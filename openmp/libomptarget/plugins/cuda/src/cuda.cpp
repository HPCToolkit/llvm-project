//===----RTLs/cuda/src/cuda.cpp------------------------------------ C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// CUDA interface for NVIDIA acclerator
//
//===----------------------------------------------------------------------===//

//******************************************************************************
// system include files
//******************************************************************************

#include <map>
#include <iostream>

#include <string.h>

#include <cuda_runtime_api.h>


//******************************************************************************
// local include files
//******************************************************************************

#include "rtl.h"
#include "cuda.hpp"

#undef DEBUGP
#define DEBUGP(prefix, ...)                                                    \
  {                                                                            \
    fprintf(stderr, "%s --> ", prefix);                                        \
    fprintf(stderr, __VA_ARGS__);                                              \
  }

#include <inttypes.h>
#define DPxMOD "0x%0*" PRIxPTR
#define DPxPTR(ptr) ((int)(2*sizeof(uintptr_t))), ((uintptr_t) (ptr))


//******************************************************************************
// macros
//******************************************************************************

#define DEVICE_TYPE_NCHARS 1024

#define FOREACH_CUDA_RESULT(macro)   \
  macro(CUDA_SUCCESS)		     \
  macro(CUDA_ERROR_DEINITIALIZED)    \
  macro(CUDA_ERROR_NOT_INITIALIZED)  \
  macro(CUDA_ERROR_INVALID_CONTEXT)  \
  macro(CUDA_ERROR_INVALID_VALUE)    \
  macro(CUDA_ERROR_INVALID_DEVICE)


#define CUDA_RESULT_CALL(fn, args)					\
  {									\
    CUresult result = fn args;						\
    if (result != CUDA_SUCCESS) {					\
      cuda_result_report(result, #fn);					\
    }                                                                   \
  }


#define CUDA_ERROR_CALL(fn, args)					\
{                                                                       \
    cudaError_t status = fn args;                                       \
    if (status != cudaSuccess) {                                        \
        cuda_error_report(status, #fn);                                 \
    }                                                                   \
}


#define COMPUTE_CAPABILITY_EXCEEDS(properties, major_val, minor_val)	\
    (properties->major >= major_val) && (properties->minor >= minor_val)



//******************************************************************************
// types
//******************************************************************************

typedef std::map<int32_t, const char *> device_names_map_t; 


typedef void (*cuda_error_callback_t) 
(
 const char *type, 
 const char *fn, 
 const char *error_string
);


//******************************************************************************
// forward declarations 
//******************************************************************************

static void
cuda_error_callback_dummy
(
 const char *type, 
 const char *fn, 
 const char *error_string
);




//******************************************************************************
// static data
//******************************************************************************


static cuda_error_callback_t cuda_error_callback = cuda_error_callback_dummy;



//******************************************************************************
// internal operations 
//******************************************************************************

static void
cuda_error_callback_dummy
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


static const char *
cuda_result_string(CUresult result)
{
#define CUDA_RESULT_TO_STRING(r) if (r == result) return #r;

  FOREACH_CUDA_RESULT(CUDA_RESULT_TO_STRING);

#undef CUDA_RESULT_TO_STRING

  return "CUDA_RESULT_UNKNOWN";
}


static void
cuda_result_report
(
 CUresult result, 
 const char *fn
)
{
  cuda_error_callback("CUDA result error", fn, cuda_result_string(result));
}


static void
cuda_error_report
(
 cudaError_t error, 
 const char *fn
)
{
  cuda_error_callback("CUDA error", fn, cudaGetErrorString(error));
} 


static bool
cuda_device_properties
(
 cudaDeviceProp *properties,
  int device
 )
{
    cudaError_t status = cudaGetDeviceProperties(properties, device);
    return (status == cudaSuccess);
}


static int
cuda_device_capability_sampling
(
 cudaDeviceProp *properties
) 
{
  return COMPUTE_CAPABILITY_EXCEEDS(properties, 5, 2);
}



//******************************************************************************
// interface functions
//******************************************************************************

bool
cuda_initialize()
{
  static bool initialized = false;
  static bool success = false;

  if (!initialized) {
    CUresult result = cuInit(0);
    success = (result == CUDA_SUCCESS);
    if (!success) {
      cuda_result_report(result, "cuInit");
    }
    initialized = true;
  }
  return success;
}


bool
cuda_context_set
(
 CUcontext context
)
{
  DP("enter cuda_context_set(context=%p)\n", context); 

  CUresult result = cuCtxSetCurrent(context);
  bool success = (result == CUDA_SUCCESS);
  if (!success) {
    cuda_result_report(result, "cuCtxSetCurrent");
  }

  DP("exit cuda_context_set returns %d\n", success); 

  return success; 
}


const char *
cuda_device_get_name
(
 int32_t device_id
)
{
  static device_names_map_t device_names_map;
  device_names_map_t::iterator it = device_names_map.find(device_id);
  
  const char *name = 0;
  if (it != device_names_map.end()) {
    name = it->second;
  } else {
    CUdevice device_handle;
    CUresult result = cuDeviceGet(&device_handle, device_id);
    if (result == CUDA_SUCCESS) {
      char device_type[DEVICE_TYPE_NCHARS];
      CUDA_RESULT_CALL(cuDeviceGetName, 
		       (device_type, DEVICE_TYPE_NCHARS, device_handle));

      name = strdup(device_type);

      std::pair<int32_t, const char *> value(device_id, name);
      device_names_map.insert(value);
    }
  }
  return name;
}


bool
cuda_compute_capability
(
 int device,
 int *major,
 int *minor
)
{
  cudaDeviceProp properties;
  bool available = cuda_device_properties(&properties, device);
  if (available) {
    *major = properties.major;
    *minor = properties.minor;
  }
  return available;
}


int
cuda_device_supports_sampling
(
 int device
)
{
  cudaDeviceProp properties;
  cuda_device_properties(&properties, device);
  return cuda_device_capability_sampling(&properties);
}

