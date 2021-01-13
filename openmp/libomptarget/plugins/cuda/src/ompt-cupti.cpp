//===----RTLs/cuda/src/rtl.cpp - Target RTLs Implementation ------- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// OMPT for NVIDIA GPU 
//
//===----------------------------------------------------------------------===//



//******************************************************************************
// global includes 
//******************************************************************************

#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include <atomic>

#include <dlfcn.h>

#include <ompt.h>

//******************************************************************************
// local includes 
//******************************************************************************

#include "omptarget.h"

#include "rtl.h"

#include "cupti.hpp"
#include "cuda.hpp"
#include "ompt-cupti.hpp"

//******************************************************************************
// macros
//******************************************************************************

#define NO_DEVICE -1

#define DEVICE_TYPE_NCHARS 1024

#define DECLARE_CAST(t, x, y) t *x = (t *) y

#define COPY_TIMES(dest, src)			\
  {						\
    dest->start_time = src->start;		\
    dest->end_time = src->end;			\
  }

typedef enum {
  cupti_tracing_uninitialized = 0, 
  cupti_tracing_initialized = 1,
  cupti_tracing_started = 2,
  cupti_tracing_paused = 3,
  cupti_tracing_finalized = 4
} cupti_tracing_status_t;


#define OMPT_TRACING_OK      4
#define OMPT_TRACING_FAILED  2

#define OMPT_TRACING_ALL   4
#define OMPT_TRACING_SOME  3
#define OMPT_TRACING_NONE  1
#define OMPT_TRACING_ERROR 0


#define FOREACH_FLAGS(macro)						\
  macro(ompt_native_data_motion_explicit, data_motion_explicit_activities) \
  macro(ompt_native_data_motion_implicit, data_motion_implicit_activities) \
  macro(ompt_native_kernel_invocation, kernel_invocation_activities)	\
  macro(ompt_native_kernel_execution, kernel_execution_activities)	\
  macro(ompt_native_driver, driver_activities)				\
  macro(ompt_native_runtime, runtime_activities)			\
  macro(ompt_native_overhead, overhead_activities)


#define FOREACH_TARGET_FN(macro) 		\
  macro(ompt_get_device_time)			\
  macro(ompt_translate_time)			\
  macro(ompt_set_trace_native)			\
  macro(ompt_start_trace)			\
  macro(ompt_pause_trace)			\
  macro(ompt_stop_trace)			\
  macro(ompt_advance_buffer_cursor)		\
  macro(ompt_get_record_type)			\
  macro(ompt_get_record_native)			\
  macro(ompt_get_record_abstract)   \
  macro(ompt_set_pc_sampling)


#define fnptr_to_ptr(x) ((void *) (uint64_t) x)


#define ompt_ptr_unknown ((void *) 0)



//******************************************************************************
// types
//******************************************************************************

class ompt_device_info_t {
 public:
  int initialized;
  int relative_id;
  int global_id;

  CUcontext context;

  int monitoring_flags_set;
  cupti_tracing_status_t cupti_state;

  ompt_callback_buffer_request_t request_callback;
  ompt_callback_buffer_complete_t complete_callback;

  bool load_handlers_registered;
  bool paused;

  std::atomic<uint64_t> cupti_active_count;

  ompt_device_info_t() : 
    initialized(0), relative_id(0), global_id(0), context(0), 
    monitoring_flags_set(0), cupti_state(cupti_tracing_uninitialized),
    request_callback(0), complete_callback(0),
    load_handlers_registered(false), paused(false),
    cupti_active_count(0) {
  };
}; 

// this wrapper is needed around a vector type because we have
// observed a static destructor for a global vector be called by an
// at_exit handler before we are done with the vector, which may be
// needed for cleanup during processing of at_exit handlers. here, we
// don't declare a destructor so the data continues to be available.

class device_info_t {
public:
  device_info_t() : data(0) {}
  void resize(unsigned int n) {
    data = new std::vector<ompt_device_info_t>(n);
  }
  unsigned int size() {
    return data->size();
  }
  ompt_device_info_t &operator[](int n) {
    return (*data)[n];
  }
private:
  std::vector<ompt_device_info_t> *data;
};

typedef std::map<int32_t, const char *> device_types_map_t; 


//******************************************************************************
// constants
//******************************************************************************

static const char *ompt_documentation =
  #include "ompt-documentation.h"
  ;



//******************************************************************************
// global data
//******************************************************************************

//--------------------------
// values set by initializer
//--------------------------


static bool ompt_enabled = false;
static bool ompt_initialized = false;

static ompt_get_target_info_t libomptarget_get_target_info;

#define declare_name(name)			\
  static name ## _t name ## _fn = 0; 

FOREACH_OMPT_TARGET_CALLBACK(declare_name)

#undef declare_name

static device_info_t device_info;

static std::atomic<uint64_t> cupti_correlation_count;

//----------------------------------------
// thread local data
//----------------------------------------

thread_local ompt_record_abstract_t ompt_record_abstract;
thread_local ompt_id_t ompt_correlation_id;

thread_local int code_device_global_id;
thread_local int code_device_relative_id;
thread_local const char *code_path;
thread_local void *code_host_addr;


//******************************************************************************
// forward declarations 
//******************************************************************************

static bool
ompt_stop_trace
(
 ompt_device_t *device
);


//******************************************************************************
// private operations
//******************************************************************************

static inline ompt_device_info_t *
ompt_device_info
(
 ompt_device_t *device
) 
{
  return (ompt_device_info_t*) device;
}


static inline int
ompt_device_to_id
(
 ompt_device_t *device
) 
{
  ompt_device_info_t *di = ompt_device_info(device);
  return di ? di->relative_id : NO_DEVICE;
}


static ompt_device_info_t * 
ompt_device_info_from_id
(
 int device_id
)
{ 
  return &device_info[device_id];
} 


static ompt_device_t * 
ompt_device_from_id
(
 int device_id
)
{ 
  return (ompt_device_t *) ompt_device_info_from_id(device_id);
} 

// device needs
//   local device id
//   OpenMP device id
//   device name
//----------------------------------------
// OMPT initialization
//----------------------------------------


static const char *
ompt_device_get_type
(
 int32_t device_id
)
{
  static device_types_map_t device_types_map;
  device_types_map_t::iterator it = device_types_map.find(device_id);
  
  const char *name;
  if (it != device_types_map.end()) {
    name = it->second;
  } else {
    int major, minor;
    char device_type[DEVICE_TYPE_NCHARS];

    const char *device_name = cuda_device_get_name(device_id);
    cuda_compute_capability(device_id, &major, &minor);

    sprintf(device_type, 
	    "NVIDIA; %s; Compute Capability %d.%d", 
	    device_name, major, minor);

    name = strdup(device_type);

    std::pair<int32_t, const char *> value(device_id, name);
    device_types_map.insert(value);
  }
  return name;
}


static int
ompt_device_rtl_init
(
 ompt_function_lookup_t lookup,
 int initial_device_num,
 ompt_data_t *tool_data
)
{
  DP("enter ompt_device_rtl_init\n");

  ompt_enabled = true;

  libomptarget_get_target_info = 
    (ompt_get_target_info_t) lookup("libomptarget_get_target_info");

  DP("libomptarget_get_target_info = %p\n", fnptr_to_ptr(libomptarget_get_target_info));

#define ompt_bind_callback(fn) \
  fn ## _fn = (fn ## _t ) lookup(#fn); \
  DP("%s=%p\n", #fn, fnptr_to_ptr(fn ## _fn));

  FOREACH_OMPT_TARGET_CALLBACK(ompt_bind_callback)

#undef ompt_bind_callback
  
  DP("exit ompt_device_rtl_init\n");

  return 0;
}


static void
ompt_device_rtl_fini
(
 ompt_data_t *tool_data
)
{
  ompt_fini();
}


static void 
ompt_device_infos_alloc
(
 int num_devices 
)
{
  device_info.resize(num_devices);
}


static bool 
ompt_device_info_init
(
 int relative_id, 
 int global_id,
 CUcontext context
)
{
  bool valid_device = relative_id < (int) device_info.size();

  if (valid_device) {
    device_info[relative_id].relative_id = relative_id;
    device_info[relative_id].global_id = global_id;
    device_info[relative_id].context = context;
    device_info[relative_id].initialized = 1;
  }

  return valid_device;
}

//----------------------------------------
// OMPT buffer management support
//----------------------------------------

static ompt_record_abstract_t *
ompt_abstract_init()
{
  ompt_record_abstract_t *a = &ompt_record_abstract;
  a->rclass = ompt_record_native_event;
  a->hwid = ompt_hwid_none;
  a->start_time = 0;
  a->end_time = 0;
  return a;
}


//----------------------------------------
// OMPT buffer management interface
//----------------------------------------


static int32_t
ompt_advance_buffer_cursor
(
 ompt_buffer_t *buffer,
 size_t size,
 ompt_buffer_cursor_t current,
 ompt_buffer_cursor_t *next
)
{
  DP("enter ompt_advance_buffer_cursor(buffer=%p, size=%lu)\n", 
     buffer, size); 
  DECLARE_CAST(CUpti_Activity, cursor, current);
  bool result = cupti_buffer_cursor_advance((uint8_t *) buffer, size, &cursor);
  if (result) {
    *next = (ompt_buffer_cursor_t) cursor;
  }
  return result;
}


static ompt_record_t
ompt_get_record_type
(
  ompt_buffer_t *buffer,
  size_t validSize,
  ompt_buffer_cursor_t current
)
{
  DECLARE_CAST(CUpti_Activity, activity, current);
  return (cupti_buffer_cursor_isvalid((uint8_t *) buffer, validSize, activity) ?
	  ompt_record_native : ompt_record_invalid);
}


static void*
ompt_get_record_native
(
 ompt_buffer_t *buffer,
 ompt_buffer_cursor_t current,
 ompt_id_t *host_opid
)
{
  DECLARE_CAST(CUpti_Activity, activity, current);
  if (activity->kind == CUPTI_ACTIVITY_KIND_CONTEXT){
    DECLARE_CAST(CUpti_ActivityContext, context, current);
    ompt_correlation_id = context->contextId;
  }
  *host_opid = ompt_correlation_id;
  return activity;
}


static ompt_record_abstract_t * 
ompt_get_record_abstract
(
  CUpti_Activity *activity
)
{
  ompt_record_abstract_t *abs = ompt_abstract_init();

  switch (activity->kind) {

  case CUPTI_ACTIVITY_KIND_MEMCPY: {
    DECLARE_CAST(CUpti_ActivityMemcpy, rhs, activity);
    abs->type = "MEMCPY EXPLICIT";
    COPY_TIMES(abs, rhs);
    break;
  }

  case CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER: {
    DECLARE_CAST(CUpti_ActivityUnifiedMemoryCounter2, rhs, activity);
    abs->type = "MEMCPY IMPLICIT";
    COPY_TIMES(abs, rhs);
    break;
  }

  case CUPTI_ACTIVITY_KIND_KERNEL:
  case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL: {
    DECLARE_CAST(CUpti_ActivityKernel3, rhs, activity);
    abs->type = "KERNEL INVOCATION";
    COPY_TIMES(abs, rhs);
    break;
  }

  case CUPTI_ACTIVITY_KIND_SOURCE_LOCATOR: {
    abs->type = "KERNEL SOURCE LOCATOR";
    abs->rclass = ompt_record_native_info;
    break;
  }

  case CUPTI_ACTIVITY_KIND_PC_SAMPLING: {
    abs->type = "KERNEL PC SAMPLE";
    break;
  }

  case CUPTI_ACTIVITY_KIND_PC_SAMPLING_RECORD_INFO: {
    abs->type = "KERNEL PC SAMPLING INFO";
    break;
  }

  case CUPTI_ACTIVITY_KIND_FUNCTION: {
    abs->type = "FUNCTION";
    abs->rclass = ompt_record_native_info;
    break;
  }		

  case CUPTI_ACTIVITY_KIND_DRIVER: {
    DECLARE_CAST(CUpti_ActivityAPI, rhs, activity);
    abs->type = "DRIVER";
    COPY_TIMES(abs, rhs);
    abs->hwid = rhs->threadId;
    break;
  }

  case CUPTI_ACTIVITY_KIND_RUNTIME: {
    DECLARE_CAST(CUpti_ActivityAPI, rhs, activity);
    abs->type = "RUNTIME";
    COPY_TIMES(abs, rhs);
    abs->hwid = rhs->threadId;
    break;
  }

  case CUPTI_ACTIVITY_KIND_OVERHEAD: {
    DECLARE_CAST(CUpti_ActivityOverhead, rhs, activity);
    abs->type = "OVERHEAD";
    COPY_TIMES(abs, rhs);
    break;
  }

  default:
    DP("CUPTI activity kind %d not handled by ompt-cupti.cpp\n", activity->kind);
    break;
  }
  return abs;
}


static void
ompt_set_pc_sampling
(
 ompt_device_t *device,
 unsigned int enable,
 unsigned int frequency
)
{
  ompt_device_info_t *di = ompt_device_info(device);
  CUcontext context = di->context;
  if (enable) {
    cupti_pc_sampling_config(context, frequency);
    cupti_pc_sampling_enable(context);
  } else {
    cupti_pc_sampling_disable(context); 
  }
}


static void
device_completion_callback
(
 uint64_t relative_device_id,
 uint8_t *ustart,
 uint8_t *uend
)
{
  const int BUFFER_NOT_OWNED = 0;
  size_t bytes = uend - ustart;

  ompt_device_info_t *di = ompt_device_info_from_id(relative_device_id);
  if (bytes != 0) {
    if (di->paused == false && di->complete_callback)
      di->complete_callback(di->global_id, (ompt_buffer_t *) ustart, bytes,
        (ompt_buffer_cursor_t) ustart, BUFFER_NOT_OWNED);
  }
}


static void 
cupti_buffer_completion_callback
(
 CUcontext context,  // NULL since CUDA 6.0
 uint32_t streamId, // unused?
 uint8_t*  buffer,  
 size_t size,  
 size_t validSize 
)
{
  DP("enter cupti_buffer_completion_callback(buffer=%p, size=%lu, validSize=%lu)\n", 
     buffer, size, validSize); 
  CUpti_Activity *activity = NULL; // signal advance to return pointer to first record

  // set activity to point to first record
  bool status = cupti_buffer_cursor_advance(buffer, validSize, &activity);

  if (status) { // so far, so good ...
    uint64_t relative_device_id = 0;
    CUpti_Activity *start = activity;
    while (status) {
      status = cupti_buffer_cursor_advance(buffer, validSize, &activity);
      
      if (activity->kind == CUPTI_ACTIVITY_KIND_CONTEXT) {
        device_completion_callback(relative_device_id, (uint8_t *)start, (uint8_t *)activity);
        start = activity;
        DECLARE_CAST(CUpti_ActivityContext, ac, activity);
        relative_device_id = ac->deviceId;
      }
    }
    device_completion_callback(relative_device_id, (uint8_t *)start, (uint8_t *)(buffer + validSize));
  }

  size_t dropped;
  cupti_get_num_dropped_records(context, streamId, &dropped);
  if (dropped != 0) {
    DP("dropped %u activity records\n", (unsigned int) dropped);
  }   
  free(buffer);
  DP("leave cupti_buffer_completion_callback\n"); 
}



//****************************************************************************
// TARGET CONTROL API
//****************************************************************************

//----------------------------------------
// OMPT device tracing control
//----------------------------------------


static void
ompt_device_unload
(
 int module_id, 
 const void *cubin, 
 size_t cubin_size
)
{
  DP("enter ompt_device_unload(module_id=%d, cubin=%p, cubin_size=%lu)\n", 
     module_id, cubin, cubin_size); 
  if (ompt_callback_device_unload_fn) {
    cupti_trace_flush();
    ompt_callback_device_unload_fn(code_device_global_id, module_id);
  }
}


static void
ompt_device_load
(
 int module_id, 
 const void *cubin, 
 size_t cubin_size
)
{
  DP("enter ompt_device_load(module_id=%d, cubin=%p, cubin_size=%lu)\n", 
     module_id, cubin, cubin_size); 
  if (ompt_callback_device_load_fn) {
    ompt_callback_device_load_fn
      (code_device_global_id, code_path, 0, code_host_addr, cubin_size, 
       (void *)cubin, NULL, module_id);
  }
}


static void
ompt_correlation_start
(
 ompt_device_info_t *di
)
{
  DP("enter ompt_correlation_start\n"); 

  if (cupti_correlation_count.fetch_add(1) == 0) {
    cupti_subscribe_callbacks();
  }
  bool &load_handlers_registered = di->load_handlers_registered;
  if (!load_handlers_registered) {
    cupti_correlation_enable(di->context, ompt_device_load, ompt_device_unload, libomptarget_get_target_info);
    load_handlers_registered = true;
  }

  DP("exit ompt_correlation_start\n"); 
}


static void
ompt_correlation_end
(
 ompt_device_info_t *di
)
{
  DP("enter ompt_correlation_end\n"); 

  if (cupti_correlation_count.fetch_add(-1) == 1) {
    cupti_unsubscribe_callbacks();
  }
  bool &load_handlers_registered = di->load_handlers_registered;
  if (load_handlers_registered) {
    cupti_correlation_disable(di->context);
    load_handlers_registered = false;
  }

  DP("exit ompt_correlation_end\n"); 
}


static int
ompt_set_trace_native
(
 ompt_device_t *device, 
 int enable, 
 int flags 
) 
{ 
  DP("enter ompt_set_trace_native(device=%p, enable=%d, flags=%d)\n", 
     (void *) device, enable, flags);

  ompt_device_info_t *di = ompt_device_info(device);

  int tracing_result = OMPT_TRACING_ERROR;

  if (di->relative_id != NO_DEVICE) {
    int result = 0;

    CUcontext context = di->context;

#define set_trace(flag, activities)					\
    if (flags & flag) {							\
      int action_result =						\
        cupti_set_monitoring(context, activities, enable);		\
      switch (action_result) {						\
      case cupti_set_all:						\
        result |= OMPT_TRACING_OK;					\
        break;								\
      case cupti_set_some:						\
        result |= OMPT_TRACING_OK;					\
      case cupti_set_none:						\
        result |= OMPT_TRACING_FAILED;					\
        break;								\
      default:								\
        assert(0);							\
      }									\
      flags ^= flag;							\
    } 

    FOREACH_FLAGS(set_trace);
#undef set_trace

    if (flags == 0) {
      if (result & OMPT_TRACING_OK) {
        tracing_result = ((result & OMPT_TRACING_FAILED) ? 
        OMPT_TRACING_SOME : OMPT_TRACING_ALL);
      } else if (result & OMPT_TRACING_FAILED) {
        tracing_result = OMPT_TRACING_NONE;
      }
    }
  }

  DP("exit ompt_set_trace_native returns %d\n", tracing_result); 

  return tracing_result; // unhandled flags
}


static bool
ompt_pause_trace
(
 ompt_device_t *device,
 int begin_pause
)
{
  ompt_device_info_t *di = ompt_device_info(device);
  CUcontext context = di->context;
  bool result = false;

  DP("enter ompt_pause_trace(device=%p, begin_pause=%d) device_id=%d\n", 
     (void *) device, begin_pause, di->global_id);

  cupti_trace_flush();

  if ((begin_pause && di->cupti_active_count.fetch_add(-1) == 1) ||
    (!begin_pause && di->cupti_active_count.fetch_add(1) == 0)) {
    cupti_trace_pause(context, begin_pause);
    // pause trace delivery for this device
    di->paused = begin_pause;
    result = true;
  }

  DP("exit ompt_pause_trace returns %d\n", result);

  return result;
}


static bool
ompt_start_trace
(
 ompt_device_t *device,
 ompt_callback_buffer_request_t request,
 ompt_callback_buffer_complete_t complete
)
{
  ompt_device_info_t *di = ompt_device_info(device);
  CUcontext context = di->context;

  DP("enter ompt_start_trace(device=%p, request=%p, complete=%p) device_id=%d\n", 
     (void *) device, fnptr_to_ptr(request), fnptr_to_ptr(complete), di->global_id);  

  // This device has been registered
  if (di->cupti_active_count.fetch_add(1) != 0) {
    return false;
  }

  di->request_callback = request;
  di->complete_callback = complete;

  cupti_trace_init(cupti_buffer_alloc, cupti_buffer_completion_callback);

  cupti_trace_start(context);

  ompt_correlation_start(di);

  return true;
}


static bool
ompt_stop_trace
(
 ompt_device_t *device
)
{
  DP("enter ompt_stop_trace\n");

  ompt_device_info_t *di = ompt_device_info(device);
  if (di->cupti_active_count.fetch_add(-1) != 0) {
    return false;
  }

  di->paused = true;
  cupti_trace_flush();

  DP("exit ompt_stop_trace\n");

  return true;
}



//*************************************************************************

//----------------------------------------
// internal functions
//----------------------------------------
  

//----------------------------------------
// OMPT device time
//----------------------------------------


#define ompt_device(d) ((ompt_device_info_t *) d)

static ompt_device_time_t 
ompt_get_device_time
(
 ompt_device_t *device
)
{
  CUcontext context = ompt_device(device)->context;
  uint64_t time;
  cupti_device_get_timestamp(context, &time);
  return (ompt_device_time_t) time; 
}


static double
ompt_translate_time
(
 ompt_device_t *device, 
 ompt_device_time_t time
)
{
  double omp_time;

  assert(0);
  // record OpenMP time when initialized

  return omp_time;
}


//******************************************************************************
// interface operations
//******************************************************************************

extern "C" {

__attribute__ (( weak ))
void
libomptarget_rtl_ompt_init
(
 ompt_start_tool_result_t *result
)
{
  // no initialization of OMPT for device-specific rtl unless 
  // libomptarget implements this function
}

}


void 
ompt_binary_load
(
 int device_id, 
 const char *load_module,
 void *host_addr
)
{
  ompt_device_info_t *di = ompt_device_info_from_id(device_id);

  code_device_global_id = di->global_id;
  code_device_relative_id = device_id;
  code_path = load_module; 
  code_host_addr = host_addr;
}


void 
ompt_binary_unload
(
 int device_id, 
 const char *load_module,
 void *host_addr
)
{
  ompt_device_info_t *di = ompt_device_info_from_id(device_id);

  code_device_global_id = di->global_id;
  code_device_relative_id = device_id;
  code_path = load_module; 
  code_host_addr = host_addr;
}

typedef void (*libomptarget_rtl_ompt_init_t) (ompt_start_tool_result_t *result);

void
ompt_init
(
 int num_devices
)
{
  static ompt_start_tool_result_t cuda_rtl_result;

  DP("enter cuda_ompt_init\n");

  if (ompt_initialized == false) {
    void *vptr = dlsym(NULL, "libomptarget_rtl_ompt_init");
    libomptarget_rtl_ompt_init_t libomptarget_rtl_ompt_init = 
      reinterpret_cast<libomptarget_rtl_ompt_init_t>(reinterpret_cast<long>(vptr));

    if (libomptarget_rtl_ompt_init) {
      cuda_rtl_result.initialize = ompt_device_rtl_init;
      cuda_rtl_result.finalize   = ompt_device_rtl_fini;

      libomptarget_rtl_ompt_init(&cuda_rtl_result);
    }
    ompt_device_infos_alloc(num_devices);
    ompt_initialized = true;
  } 

  DP("exit cuda_ompt_init\n");
}


void
ompt_fini
(
)
{
  DP("enter cuda_ompt_fini\n");

  if (ompt_initialized) {
    DP("  cuda finalization activated\n");
    if (ompt_callback_device_finalize_fn) {
      for (unsigned int i = 0; i < device_info.size(); i++) {
        if (device_info[i].initialized) {
          ompt_stop_trace(ompt_device_from_id(i));
          ompt_correlation_end(&device_info[i]);
          ompt_callback_device_finalize_fn(device_info[i].global_id); 
        }
      }
      cupti_trace_finalize();
    }

    ompt_enabled = false;
    ompt_initialized = false;
  } else {
    DP("  cuda finalization already complete\n");
  }

  DP("exit cuda_ompt_fini\n");
}


static ompt_interface_fn_t 
ompt_device_lookup
(
 const char *s
)
{
#define macro(fn) \
  if (strcmp(s, #fn) == 0) return (ompt_interface_fn_t) fn;

  FOREACH_TARGET_FN(macro);

#undef macro

  return (ompt_interface_fn_t) 0;
}


void
ompt_device_init
(
 int device_id, 
 int omp_device_id,
 CUcontext context 
)
{
  DP("enter ompt_device_init\n");

  DP("ompt_callback_device_initialize = %p\n", (void *) (uint64_t) ompt_callback_device_initialize_fn);
  if (ompt_callback_device_initialize_fn) {
    if (ompt_device_info_init(device_id, omp_device_id, context)) {

      DP("calling ompt_callback_device_initialize\n");

      ompt_callback_device_initialize_fn
        (omp_device_id,
         ompt_device_get_type(omp_device_id),
         ompt_device_from_id(device_id),
         ompt_device_lookup,
         ompt_documentation);
    }
  }

  DP("exit ompt_device_init\n");
}
