//===----RTLs/cuda/src/ompt-cupti.hpp - OMPT for GPUs using CUPTI - C++ -*-===//
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

#ifndef __OMPT_CUPTI__
#define __OMPT_CUPTI__

#include <cuda.h>

extern void
ompt_init
(
  int num_devices
);


extern void
ompt_fini
(
);


extern void
ompt_device_init
(
 int device_id, 
 int omp_device_id,
 CUcontext context
);


extern void
ompt_binary_load
(
 int device_id, 
 const char *load_module,
 void *addr
);


extern void
ompt_binary_unload
(
 int device_id, 
 const char *load_module,
 void *addr
);


#endif
