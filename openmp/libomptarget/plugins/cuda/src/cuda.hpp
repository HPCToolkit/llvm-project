//===----RTLs/cuda/src/cuda.hpp------------------------------------ C++ -*-===//
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
#ifndef __CUDA_HPP__

//******************************************************************************
// include files
//******************************************************************************

#include <cuda.h>



//******************************************************************************
// interface functions
//******************************************************************************

bool
cuda_context_set(CUcontext context);


const char *
cuda_device_get_name
(
 int32_t device_id
);



bool
cuda_compute_capability
(
 int device,
 int *major,
 int *minor
);
    

//===----------------------------------------------------------------------===//
#endif // __CUDA_HPP__
