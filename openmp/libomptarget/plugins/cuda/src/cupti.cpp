//===----RTLs/cuda/src/cupti.cpp----------------------------------- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// CUPTI interface for NVIDIA GPU
//
//===----------------------------------------------------------------------===//


//******************************************************************************
// system include files 
//******************************************************************************

#include <iostream>
#include <map>
#include <set>

#include <stdio.h>
#include <stdlib.h>


//******************************************************************************
// local include files
//******************************************************************************

#undef DEBUGP
#define DEBUGP(prefix, ...)                                                    \
  {                                                                            \
    fprintf(stderr, "%s --> ", prefix);                                        \
    fprintf(stderr, __VA_ARGS__);                                              \
  }

#include <inttypes.h>
#define DPxMOD "0x%0*" PRIxPTR
#define DPxPTR(ptr) ((int)(2*sizeof(uintptr_t))), ((uintptr_t) (ptr))

#include "rtl.h" 

#undef DP
#ifdef OMPTARGET_CUPTI_DEBUG
#define DP(...) DEBUGP("cupti ", __VA_ARGS__)
#else
#define DP(...)
#endif


//******************************************************************************
// cuda include 
//******************************************************************************

#include <cupti.h> 

#include "cupti.hpp" 
#include "cuda.hpp" 

  

//******************************************************************************
// macros
//******************************************************************************

#define CUPTI_ACTIVITY_BUFFER_SIZE (64 * 1024)

#define CUPTI_ACTIVITY_BUFFER_ALIGNMENT (8)

#define CUPTI_CALL(fn, args) \
{      \
    CUptiResult status = fn args; \
    if (status != CUPTI_SUCCESS) { \
      cupti_error_report(status, #fn); \
    }\
}

#define DISPATCH_CALLBACK(fn, args) if (fn) fn args


//******************************************************************************
// types
//******************************************************************************


typedef void (*cupti_error_callback_t) 
(
 const char *type, 
 const char *fn, 
 const char *error_string
);


typedef void (*cupti_dropped_callback_t) 
(
 size_t dropped
);


typedef CUptiResult (*cupti_activity_enable_disable_t) 
(
 CUcontext context,
 CUpti_ActivityKind activity
);


typedef struct {
  CUpti_BuffersCallbackRequestFunc buffer_request; 
  CUpti_BuffersCallbackCompleteFunc buffer_complete;
} cupti_activity_buffer_state_t;



//******************************************************************************
// forward declarations 
//******************************************************************************

static void
cupti_error_callback_dummy
(
 const char *type, 
 const char *fn, 
 const char *error_string
);


static int 
cupti_correlation_callback_dummy
(
 uint64_t *device_num,
 uint64_t *target_id,
 uint64_t *host_op_id
);



//******************************************************************************
// constants
//******************************************************************************

CUpti_ActivityKind
external_correlation_activities[] = {
  CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION, 
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
data_motion_explicit_activities[] = {
  CUPTI_ACTIVITY_KIND_MEMCPY2,
  CUPTI_ACTIVITY_KIND_MEMCPY, 
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
data_motion_implicit_activities[] = {
  CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER,
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
kernel_invocation_activities[] = {
  CUPTI_ACTIVITY_KIND_KERNEL,
  CUPTI_ACTIVITY_KIND_SYNCHRONIZATION,
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
kernel_execution_activities[] = {
  CUPTI_ACTIVITY_KIND_CONTEXT,
  CUPTI_ACTIVITY_KIND_FUNCTION,
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
overhead_activities[] = {
  CUPTI_ACTIVITY_KIND_OVERHEAD,
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
driver_activities[] = {
  CUPTI_ACTIVITY_KIND_DEVICE,
  CUPTI_ACTIVITY_KIND_DRIVER,
  CUPTI_ACTIVITY_KIND_INVALID
};


CUpti_ActivityKind
runtime_activities[] = {
  CUPTI_ACTIVITY_KIND_DEVICE,
  CUPTI_ACTIVITY_KIND_RUNTIME,
  CUPTI_ACTIVITY_KIND_INVALID
};



//******************************************************************************
// static data
//******************************************************************************
//
static std::map<CUcontext, std::map<CUpti_ActivityKind, bool> > cupti_enabled_activities;
static std::map<CUcontext, bool> cupti_enabled_pc_sampling;
static bool cupti_enabled_correlation = false;

static cupti_correlation_callback_t cupti_correlation_callback = 
  cupti_correlation_callback_dummy;

static cupti_error_callback_t cupti_error_callback = 
  cupti_error_callback_dummy;

static cupti_activity_buffer_state_t cupti_activity_enabled = { 0, 0 };
static cupti_activity_buffer_state_t cupti_activity_disabled = { 0, 0 };

static cupti_activity_buffer_state_t *cupti_activity_state = 
  &cupti_activity_disabled;

static cupti_load_callback_t cupti_load_callback = 0;

static cupti_load_callback_t cupti_unload_callback = 0;

static CUpti_SubscriberHandle cupti_subscriber;


//******************************************************************************
// private operations
//******************************************************************************

static void
cupti_error_callback_dummy // __attribute__((unused))
(
 const char *type, 
 const char *fn, 
 const char *error_string
)
{
  std::cerr << type << ": function " << fn
    << " failed with error " << error_string << std::endl;                       
  exit(-1);
} 


static void
cupti_error_report
(
 CUptiResult error, 
 const char *fn
)
{
  const char *error_string;
  cuptiGetResultString(error, &error_string);
  cupti_error_callback("CUPTI result error", fn, error_string);
} 


//******************************************************************************
// internal functions
//******************************************************************************

static void
cupti_subscriber_callback
(
 void *userdata,
 CUpti_CallbackDomain domain,
 CUpti_CallbackId cb_id,
 const CUpti_CallbackData *cb_info
)
{
  DP("enter cupti_subscriber_callback\n");

  if (domain == CUPTI_CB_DOMAIN_RESOURCE) {
    const CUpti_ResourceData *rd = (const CUpti_ResourceData *) cb_info;
    if (cb_id == CUPTI_CBID_RESOURCE_MODULE_LOADED) {
      CUpti_ModuleResourceData *mrd = (CUpti_ModuleResourceData *) rd->resourceDescriptor;
      DP("loaded module id %d, cubin size %ld, cubin %p\n", 
        mrd->moduleId, mrd->cubinSize, mrd->pCubin);
      DISPATCH_CALLBACK(cupti_load_callback, (mrd->moduleId, mrd->pCubin, mrd->cubinSize));
    }
    if (cb_id == CUPTI_CBID_RESOURCE_MODULE_UNLOAD_STARTING) {
      CUpti_ModuleResourceData *mrd = (CUpti_ModuleResourceData *) rd->resourceDescriptor;
      DP("unloaded module id %d, cubin size %ld, cubin %p\n", 
        mrd->moduleId, mrd->cubinSize, mrd->pCubin);
      DISPATCH_CALLBACK(cupti_unload_callback, (mrd->moduleId, mrd->pCubin, mrd->cubinSize));
    }
  } else if (domain == CUPTI_CB_DOMAIN_DRIVER_API) {
    switch (cb_id) {
      case CUPTI_DRIVER_TRACE_CBID_cuCtxSynchronize:
      case CUPTI_DRIVER_TRACE_CBID_cuEventSynchronize:
      case CUPTI_DRIVER_TRACE_CBID_cuStreamSynchronize:
      case CUPTI_DRIVER_TRACE_CBID_cuStreamSynchronize_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuStreamWaitEvent:
      case CUPTI_DRIVER_TRACE_CBID_cuStreamWaitEvent_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemAlloc:
      case CUPTI_DRIVER_TRACE_CBID_cu64MemAlloc:
      case CUPTI_DRIVER_TRACE_CBID_cuMemAllocPitch:
      case CUPTI_DRIVER_TRACE_CBID_cu64MemAllocPitch:
      case CUPTI_DRIVER_TRACE_CBID_cuMemAlloc_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemAllocPitch_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DUnaligned:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoAAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoHAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoHAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DUnaligned_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoAAsync_v2:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeer:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeerAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DPeer:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DPeerAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DUnaligned_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeer_ptds:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoAAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoHAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeerAsync_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DPeerAsync_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunch:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchGrid:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchGridAsync:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel_ptsz:
      case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice:
        {
          uint64_t correlation_id;
          // TODO(Keren): for now, not care about region id and device num
          DISPATCH_CALLBACK(cupti_correlation_callback, (NULL, NULL, &correlation_id));

          if (correlation_id != 0) {
            if (cb_info->callbackSite == CUPTI_API_ENTER) {
              CUPTI_CALL(cuptiActivityPushExternalCorrelationId,
                (CUPTI_EXTERNAL_CORRELATION_KIND_UNKNOWN, correlation_id));
              DP("Driver push externalId %lu (cb_id = %u)\n", correlation_id, cb_id);
            }
            if (cb_info->callbackSite == CUPTI_API_EXIT) {
              CUPTI_CALL(cuptiActivityPopExternalCorrelationId,
                (CUPTI_EXTERNAL_CORRELATION_KIND_UNKNOWN, &correlation_id));
              DP("Driver pop externalId %lu (cb_id = %u)\n", correlation_id, cb_id);
            }
          }
        }
    }
  } else if (domain == CUPTI_CB_DOMAIN_RUNTIME_API) { 
    switch (cb_id) {
      case CUPTI_RUNTIME_TRACE_CBID_cudaEventSynchronize_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaStreamSynchronize_ptsz_v7000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaStreamWaitEvent_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaDeviceSynchronize_v3020: 
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunch_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunchKernel_v7000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunch_ptsz_v7000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunchKernel_ptsz_v7000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMalloc_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMallocPitch_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMallocArray_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMalloc3D_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMalloc3DArray_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyPeer_v4000:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyPeerAsync_v4000:       
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeer_v4000:          
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeerAsync_v4000:     
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3D_v3020:              
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DAsync_v3020:         
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3D_ptds_v7000:         
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DAsync_ptsz_v7000:    
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeer_ptds_v7000:    
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy3DPeerAsync_ptsz_v7000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2D_v3020:             
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArray_v3020:         
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DToArray_v3020:       
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArray_v3020:       
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DFromArray_v3020:     
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyArrayToArray_v3020:    
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DArrayToArray_v3020:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbol_v3020:        
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbol_v3020:      
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyAsync_v3020:           
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArrayAsync_v3020:    
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArrayAsync_v3020:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DAsync_v3020:         
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DToArrayAsync_v3020:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DFromArrayAsync_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbolAsync_v3020:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbolAsync_v3020:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy_ptds_v7000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2D_ptds_v7000:            
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArray_ptds_v7000:        
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DToArray_ptds_v7000:       
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArray_ptds_v7000:       
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DFromArray_ptds_v7000:     
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyArrayToArray_ptds_v7000:    
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DArrayToArray_ptds_v7000:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbol_ptds_v7000:        
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbol_ptds_v7000:      
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyAsync_ptsz_v7000:           
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToArrayAsync_ptsz_v7000:    
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromArrayAsync_ptsz_v7000:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DAsync_ptsz_v7000:         
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DToArrayAsync_ptsz_v7000:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpy2DFromArrayAsync_ptsz_v7000:                              
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyToSymbolAsync_ptsz_v7000:  
      case CUPTI_RUNTIME_TRACE_CBID_cudaMemcpyFromSymbolAsync_ptsz_v7000: 
      #if CUPTI_API_VERSION == 10
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunchCooperativeKernel_v9000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunchCooperativeKernel_ptsz_v9000:
      case CUPTI_RUNTIME_TRACE_CBID_cudaLaunchCooperativeKernelMultiDevice_v9000:  
      #endif
      {
        uint64_t correlation_id;
        // TODO(Keren): for now, not care about region id and device num
        DISPATCH_CALLBACK(cupti_correlation_callback, (NULL, NULL, &correlation_id));

        if (correlation_id != 0) {
          if (cb_info->callbackSite == CUPTI_API_ENTER) {
            DP("Runtime push externalId %lu (cb_id = %u)\n", correlation_id, cb_id);
            CUPTI_CALL(cuptiActivityPushExternalCorrelationId, (CUPTI_EXTERNAL_CORRELATION_KIND_UNKNOWN, correlation_id));
          }
          if (cb_info->callbackSite == CUPTI_API_EXIT) {
            CUPTI_CALL(cuptiActivityPopExternalCorrelationId, (CUPTI_EXTERNAL_CORRELATION_KIND_UNKNOWN, &correlation_id));
            DP("Runtime pop externalId %lu (cb_id = %u)\n", correlation_id, cb_id);
          }
        }
        break;
      }
      default:
        break;
    }
  }

  DP("exit cupti_subscriber_callback\n");
}


static int 
cupti_correlation_callback_dummy // __attribute__((unused))
(
 uint64_t *device_num,
 uint64_t *target_id,
 uint64_t *host_op_id
)
{
  *host_op_id = 0;
  return 0;
}


//******************************************************************************
// interface  operations
//******************************************************************************


void
cupti_device_get_timestamp
(
 CUcontext context,
 uint64_t *time
)
{
  CUPTI_CALL(cuptiDeviceGetTimestamp, (context, time));
}


void 
cupti_buffer_alloc 
(
 uint8_t **buffer, 
 size_t *buffer_size, 
 size_t *maxNumRecords
)
{
  int retval = posix_memalign((void **) buffer,
    (size_t) CUPTI_ACTIVITY_BUFFER_ALIGNMENT,
    (size_t) CUPTI_ACTIVITY_BUFFER_SIZE); 
  
  if (retval != 0) {
    cupti_error_callback("CUPTI", "cupti_buffer_alloc", "out of memory");
  }
  
  *buffer_size = CUPTI_ACTIVITY_BUFFER_SIZE;

  *maxNumRecords = 0;
}

//-------------------------------------------------------------
// event specification
//-------------------------------------------------------------

cupti_set_status_t
cupti_set_monitoring
(
 CUcontext context,
 const CUpti_ActivityKind activity_kinds[],
 bool enable
)
{
  DP("enter cupti_set_monitoring\n");
  int failed = 0;
  int succeeded = 0;
  cupti_activity_enable_disable_t action =
    (enable ? cuptiActivityEnableContext : cuptiActivityDisableContext);
  int i = 0;
  for (;;) {
    CUpti_ActivityKind activity_kind = activity_kinds[i++];
    if (activity_kind == CUPTI_ACTIVITY_KIND_INVALID) break;
    if ((enable && cupti_enabled_activities[context][activity_kind]) ||
      (!enable && !cupti_enabled_activities[context][activity_kind])) {
      succeeded++;
      continue;
    }
    bool succ = action(context, activity_kind) == CUPTI_SUCCESS;
    if (succ) {
      if (enable) {
        DP("activity %d enabled\n", activity_kind);
        cupti_enabled_activities[context][activity_kind] = true;
      } else {
        DP("activity %d disabled\n", activity_kind);
        cupti_enabled_activities[context][activity_kind] = false;
      }
      succeeded++;
    }
    else failed++;
  }
  if (succeeded > 0) {
    if (failed == 0) return cupti_set_all;
    else return cupti_set_some;
  }
  DP("leave cupti_set_monitoring\n");
  return cupti_set_none;
}


//-------------------------------------------------------------
// tracing control 
//-------------------------------------------------------------

void 
cupti_trace_init
(
 CUpti_BuffersCallbackRequestFunc buffer_request, 
 CUpti_BuffersCallbackCompleteFunc buffer_complete
)
{
  cupti_activity_enabled.buffer_request = buffer_request;
  cupti_activity_enabled.buffer_complete = buffer_complete;
}


void
cupti_trace_flush
(
)
{
  CUPTI_CALL(cuptiActivityFlushAll, (CUPTI_ACTIVITY_FLAG_FLUSH_FORCED));
}


void 
cupti_trace_start
(
 CUcontext context
)
{
  *cupti_activity_state = cupti_activity_enabled;
  CUPTI_CALL(cuptiActivityRegisterCallbacks,
    (cupti_activity_state->buffer_request, cupti_activity_state->buffer_complete));
}


void 
cupti_trace_pause
(
 CUcontext context,
 bool begin_pause
)
{
  cupti_activity_enable_disable_t action =
    (begin_pause ? cuptiActivityDisableContext : cuptiActivityEnableContext);
  for (auto it = cupti_enabled_activities[context].begin(); it != cupti_enabled_activities[context].end(); ++it) {
    CUpti_ActivityKind activity = it->first;
    bool enabled = it->second;
    if (begin_pause == enabled) {
      bool activity_succ = action(context, activity) == CUPTI_SUCCESS;
      if (activity_succ) {
        it->second = !enabled;
      }
    }
  }
  bool enabled = cupti_enabled_pc_sampling[context];
  if (begin_pause == enabled) {
    bool activity_succ = action(context, CUPTI_ACTIVITY_KIND_PC_SAMPLING) == CUPTI_SUCCESS;
    if (activity_succ) {
      cupti_enabled_pc_sampling[context] = !enabled;
    }
  }
}


void 
cupti_trace_finalize
(
)
{
  CUPTI_CALL(cuptiFinalize, ());
}


//-------------------------------------------------------------
// correlation callback control 
//-------------------------------------------------------------

void
cupti_subscribe_callbacks
(
)
{
  CUPTI_CALL(cuptiSubscribe, (&cupti_subscriber,
    (CUpti_CallbackFunc) cupti_subscriber_callback,
    (void *) NULL));
  CUPTI_CALL(cuptiEnableDomain, (1, cupti_subscriber, CUPTI_CB_DOMAIN_DRIVER_API));
  CUPTI_CALL(cuptiEnableDomain, (1, cupti_subscriber, CUPTI_CB_DOMAIN_RUNTIME_API));
  CUPTI_CALL(cuptiEnableDomain, (1, cupti_subscriber, CUPTI_CB_DOMAIN_RESOURCE));
}


void
cupti_unsubscribe_callbacks
(
)
{
  CUPTI_CALL(cuptiUnsubscribe, (cupti_subscriber));
  CUPTI_CALL(cuptiEnableDomain, (0, cupti_subscriber, CUPTI_CB_DOMAIN_DRIVER_API));
  CUPTI_CALL(cuptiEnableDomain, (0, cupti_subscriber, CUPTI_CB_DOMAIN_RUNTIME_API));
  CUPTI_CALL(cuptiEnableDomain, (0, cupti_subscriber, CUPTI_CB_DOMAIN_RESOURCE));
}


void
cupti_correlation_enable
(
 CUcontext context, 
 cupti_load_callback_t load_callback,
 cupti_load_callback_t unload_callback,
 cupti_correlation_callback_t correlation_callback
)
{
  cupti_load_callback = load_callback;
  cupti_unload_callback = unload_callback;
  cupti_correlation_callback = correlation_callback;

  if (!cupti_enabled_correlation && cupti_correlation_callback) {
    CUPTI_CALL(cuptiActivityEnable,
      (CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
    cupti_enabled_correlation = true;
    DP("enable correlation\n");
  }
}


void
cupti_correlation_disable
(
 CUcontext context
)
{
  if (cupti_enabled_correlation) {
    CUPTI_CALL(cuptiActivityDisable, (CUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
    cupti_enabled_correlation = false;
    DP("stop correlation\n");
  }

  cupti_load_callback = 0;
  cupti_unload_callback = 0;
  cupti_correlation_callback = 0;
}


//-------------------------------------------------------------
// cursor support
//-------------------------------------------------------------
  
bool
cupti_buffer_cursor_advance
(
 uint8_t *buffer,
 size_t size,
 CUpti_Activity **activity
)
{
  return cuptiActivityGetNextRecord(buffer, size, activity) == CUPTI_SUCCESS;
}


bool
cupti_buffer_cursor_isvalid
(
 uint8_t *buffer,
 size_t size,
 CUpti_Activity *activity
)
{
  CUpti_Activity *cursor = activity;
  return cupti_buffer_cursor_advance(buffer, size, &cursor);
}


void
cupti_get_num_dropped_records
(
 CUcontext context,
 uint32_t streamId,
 size_t* dropped 
)
{
  CUPTI_CALL(cuptiActivityGetNumDroppedRecords, (context, streamId, dropped));
}

//-------------------------------------------------------------
// pc sampling support
//-------------------------------------------------------------

void
cupti_pc_sampling_config
(
 CUcontext context,
 int frequency
)
{
  CUpti_ActivityPCSamplingConfig config;
  config.size = sizeof(CUpti_ActivityPCSamplingConfig);
  config.samplingPeriod2 = frequency;
  CUPTI_CALL(cuptiActivityConfigurePCSampling, (context, &config));
}


void
cupti_pc_sampling_enable
(
 CUcontext context
)
{
  cupti_enabled_pc_sampling[context] = true;
  CUPTI_CALL(cuptiActivityEnableContext, (context, CUPTI_ACTIVITY_KIND_PC_SAMPLING));
}


void
cupti_pc_sampling_disable
(
 CUcontext context
)
{
  cupti_enabled_pc_sampling[context] = false;
  CUPTI_CALL(cuptiActivityDisableContext, (context, CUPTI_ACTIVITY_KIND_PC_SAMPLING));
}
