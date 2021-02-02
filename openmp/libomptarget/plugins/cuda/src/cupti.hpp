//===----RTLs/cuda/src/cupti.hpp----------------------------------- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// CUPTI interface for NVIDIA acclerator
//
//===----------------------------------------------------------------------===//
#ifndef __CUPTI_HPP__
#define __CUPTI_HPP__



//******************************************************************************
// include files
//******************************************************************************

#include <cupti_activity.h>



//******************************************************************************
// types
//******************************************************************************

typedef int (*cupti_correlation_callback_t)
(
 uint64_t *device_num,
 uint64_t *target_id,
 uint64_t *host_op_id
);


typedef void (*cupti_load_callback_t)
(
 int module_id, 
 const void *cubin, 
 size_t cubin_size
);



//******************************************************************************
// constants
//******************************************************************************

extern CUpti_ActivityKind
external_correlation_activities[];

extern CUpti_ActivityKind
data_motion_explicit_activities[];

extern CUpti_ActivityKind
data_motion_implicit_activities[];

extern CUpti_ActivityKind
kernel_invocation_activities[];

extern CUpti_ActivityKind
kernel_execution_activities[];

extern CUpti_ActivityKind
driver_activities[];

extern CUpti_ActivityKind
runtime_activities[];

extern CUpti_ActivityKind
overhead_activities[];

typedef enum {
  cupti_set_all = 1,
  cupti_set_some = 2,
  cupti_set_none = 3
} cupti_set_status_t;



//******************************************************************************
// interface functions
//******************************************************************************

extern void 
cupti_buffer_alloc 
(
 uint8_t **buffer, 
 size_t *buffer_size, 
 size_t *maxNumRecords
);


extern void
cupti_subscribe_callbacks
(
);


extern void
cupti_unsubscribe_callbacks
(
);


extern bool
cupti_buffer_cursor_advance
(
 uint8_t *buffer,
 size_t validSize,
 CUpti_Activity **activity
);


extern bool
cupti_buffer_cursor_isvalid
(
 uint8_t *buffer,
 size_t validSize,
 CUpti_Activity *activity
);


extern void
cupti_correlation_enable
(
 CUcontext context,
 cupti_load_callback_t load_callback,
 cupti_load_callback_t unload_callback,
 cupti_correlation_callback_t callback_fn
);


extern void
cupti_correlation_disable
(
 CUcontext context
);


extern cupti_set_status_t 
cupti_set_monitoring
(
 CUcontext context,
 const  CUpti_ActivityKind activity_kinds[],
 bool enable
);


extern void
cupti_device_get_timestamp
(
 CUcontext context,
 uint64_t *time
);


extern void 
cupti_trace_init
(
 CUpti_BuffersCallbackRequestFunc buffer_request, 
 CUpti_BuffersCallbackCompleteFunc buffer_complete
);


extern void
cupti_trace_flush
(
);


extern void 
cupti_trace_start
(
 CUcontext context
);


extern void 
cupti_trace_pause
(
 CUcontext context,
 bool begin_pause
);


extern void 
cupti_trace_finalize
(
);


extern void
cupti_get_num_dropped_records
(
 CUcontext context,
 uint32_t streamId,
 size_t* dropped 
);


extern void
cupti_pc_sampling_config
(
 CUcontext context,
 int frequency
);


extern void
cupti_pc_sampling_enable
(
 CUcontext context
);


void
cupti_pc_sampling_disable
(
 CUcontext context
);

//===----------------------------------------------------------------------===//
#endif // __CUPTI_HPP__
