/*
 * ompt-specific.cpp -- OMPT internal functions
 */

//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

//******************************************************************************
// include files
//******************************************************************************

#include "kmp.h"
#include "ompt-specific.h"

#if KMP_OS_UNIX
#include <dlfcn.h>
#endif

#if KMP_OS_WINDOWS
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

#define OMPT_WEAK_ATTRIBUTE KMP_WEAK_ATTRIBUTE_INTERNAL

//******************************************************************************
// macros
//******************************************************************************

#define LWT_FROM_TEAM(team) (team)->t.ompt_serialized_team_info

#define OMPT_THREAD_ID_BITS 16

#define LWT_STATE_REGULAR 0x0
#define LWT_STATE_LINKING 0x1
#define LWT_STATE_UNLINKING 0x2
// masking lower two bits
#define LWT_MASK_STATE(ptr, value) \
  ptr = (ompt_lw_taskteam_t *)((uint64_t)ptr | value)
// unmasking lower two bits
#define LWT_UNMASK_STATE(ptr) \
  ptr = (ompt_lw_taskteam_t *)((uint64_t)ptr & 0xFFFFFFFFFFFFFFFC)

#define LWT_GET_UNMASKED(ptr) \
  ((ompt_lw_taskteam_t *)((uint64_t)ptr & 0xFFFFFFFFFFFFFFFC))

// masking lower bit
#define PTR_MASK_LOWER_BIT(ptr, type) \
  ptr = (type *)((uint64_t)ptr | 0x1)
// unmasking lower bit
#define PTR_UNMASK_LOWER_BIT(ptr, type) \
  ptr = (type *)((uint64_t)ptr & (0xFFFFFFFFFFFFFFFE))

#define PTR_GET_UNMASKED(ptr, type) \
  ((type *)((uint64_t)ptr & (0xFFFFFFFFFFFFFFFE)))

#define PTR_HAS_MASKED_LOWER_BIT(ptr) \
  (uint64_t)ptr & 0x1

#define LWT_LINKING_IN_PROGRESS(thr) \
  (uint64_t)thr->th.th_team->t.ompt_serialized_team_info & 0x1

#define LWT_STATE_IS_ACTIVE(ptr) \
  (uint64_t)ptr & 0x3

//******************************************************************************
// private operations
//******************************************************************************

//----------------------------------------------------------
// traverse the team and task hierarchy
// note: __ompt_get_teaminfo and __ompt_get_task_info_object
//       traverse the hierarchy similarly and need to be
//       kept consistent
//----------------------------------------------------------

ompt_team_info_t *__ompt_get_teaminfo(int depth, int *size) {
  kmp_info_t *thr = ompt_get_thread();

  if (thr) {
    kmp_team *team = thr->th.th_team;
    if (team == NULL)
      return NULL;

    ompt_lw_taskteam_t *next_lwt = LWT_FROM_TEAM(team), *lwt = NULL;

    while (depth > 0) {
      // next lightweight team (if any)
      if (lwt)
        lwt = lwt->parent;

      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (!lwt && team) {
        if (next_lwt) {
          lwt = next_lwt;
          next_lwt = NULL;
        } else {
          team = team->t.t_parent;
          if (team) {
            next_lwt = LWT_FROM_TEAM(team);
          }
        }
      }

      depth--;
    }

    if (lwt) {
      // lightweight teams have one task
      if (size)
        *size = 1;

      // return team info for lightweight team
      return &lwt->ompt_info->ompt_team_info;
    } else if (team) {
      // extract size from heavyweight team
      if (size)
        *size = team->t.t_nproc;

      // return team info for heavyweight team
      return team->t.ompt_team_info;
    }
  }

  return NULL;
}

ompt_task_info_t *__ompt_get_task_info_object(int depth) {
  ompt_task_info_t *info = NULL;
  kmp_info_t *thr = ompt_get_thread();

  if (thr) {
    kmp_taskdata_t *taskdata = thr->th.th_current_task;
    ompt_lw_taskteam_t *lwt = NULL,
                       *next_lwt = LWT_FROM_TEAM(taskdata->td_team);

    while (depth > 0) {
      // next lightweight team (if any)
      if (lwt)
        lwt = lwt->parent;

      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (!lwt && taskdata) {
        if (next_lwt) {
          lwt = next_lwt;
          next_lwt = NULL;
        } else {
          taskdata = taskdata->td_parent;
          if (taskdata) {
            next_lwt = LWT_FROM_TEAM(taskdata->td_team);
          }
        }
      }
      depth--;
    }

    if (lwt) {
      info = &lwt->ompt_info->ompt_task_info;
    } else if (taskdata) {
      info = taskdata->ompt_task_info;
    }
  }

  return info;
}

ompt_task_info_t *__ompt_get_scheduling_taskinfo(int depth) {
  ompt_task_info_t *info = NULL;
  kmp_info_t *thr = ompt_get_thread();

  if (thr) {
    kmp_taskdata_t *taskdata = thr->th.th_current_task;

    ompt_lw_taskteam_t *lwt = NULL,
                       *next_lwt = LWT_FROM_TEAM(taskdata->td_team);

    while (depth > 0) {
      // next lightweight team (if any)
      if (lwt)
        lwt = lwt->parent;

      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (!lwt && taskdata) {
        // first try scheduling parent (for explicit task scheduling)
        if (taskdata->ompt_task_info->scheduling_parent) {
          taskdata = taskdata->ompt_task_info->scheduling_parent;
        } else if (next_lwt) {
          lwt = next_lwt;
          next_lwt = NULL;
        } else {
          // then go for implicit tasks
          taskdata = taskdata->td_parent;
          if (taskdata) {
            next_lwt = LWT_FROM_TEAM(taskdata->td_team);
          }
        }
      }
      depth--;
    }

    if (lwt) {
      info = &lwt->ompt_info->ompt_task_info;
    } else if (taskdata) {
      info = taskdata->ompt_task_info;
    }
  }

  return info;
}

//******************************************************************************
// interface operations
//******************************************************************************
//----------------------------------------------------------
// initialization support
//----------------------------------------------------------

void
__ompt_force_initialization()
{
  __kmp_serial_initialize();
}

//----------------------------------------------------------
// thread support
//----------------------------------------------------------

ompt_data_t *__ompt_get_thread_data_internal() {
  if (__kmp_get_gtid() >= 0) {
    kmp_info_t *thread = ompt_get_thread();
    if (thread == NULL)
      return NULL;
    return &(thread->th.ompt_thread_info.thread_data);
  }
  return NULL;
}

//----------------------------------------------------------
// state support
//----------------------------------------------------------

void __ompt_thread_assign_wait_id(void *variable) {
  kmp_info_t *ti = ompt_get_thread();

  if (ti)
    ti->th.ompt_thread_info.wait_id = (ompt_wait_id_t)(uintptr_t)variable;
}

int __ompt_get_state_internal(ompt_wait_id_t *omp_wait_id) {
  kmp_info_t *ti = ompt_get_thread();

  if (ti) {
    if (omp_wait_id)
      *omp_wait_id = ti->th.ompt_thread_info.wait_id;
    return ti->th.ompt_thread_info.state;
  }
  return ompt_state_undefined;
}

//----------------------------------------------------------
// parallel region support
//----------------------------------------------------------

int __ompt_get_parallel_info_internal(int ancestor_level,
                                      ompt_data_t **parallel_data,
                                      int *team_size) {
  if (__kmp_get_gtid() >= 0) {
    ompt_team_info_t *info;
    if (team_size) {
      info = __ompt_get_teaminfo(ancestor_level, team_size);
    } else {
      info = __ompt_get_teaminfo(ancestor_level, NULL);
    }
    if (parallel_data) {
      *parallel_data = info ? &(info->parallel_data) : NULL;
    }
    return info ? 2 : 0;
  } else {
    return 0;
  }
}

//----------------------------------------------------------
// lightweight task team support
//----------------------------------------------------------

static inline void
__ompt_end_team_info_init(ompt_team_info_t *team_info) {
  if (!team_info->end_team_info) {
    // initialized ompt_info ptr if not
    team_info->end_team_info = &team_info->end_team_info_pair[0];
  }
}

static inline void
__ompt_end_team_info_pair_init
(
    end_team_info_t *old_team_info
)
{
  old_team_info->old_parallel_data.ptr = NULL;
  old_team_info->old_master_return_address = NULL;
}


static inline void
__ompt_lw_copy_ompt_info_pair
(
  ompt_info_pair_t *ompt_pair_ptr,
  ompt_data_t *ompt_pid,
  void *codeptr
)
{
  ompt_team_info_t *team_info = &ompt_pair_ptr->ompt_team_info;
  team_info->parallel_data = *ompt_pid;
  team_info->master_return_address = codeptr;
  // set both old_parallel_data and old_master_return_address to NULL
  __ompt_end_team_info_pair_init(&team_info->end_team_info_pair[0]);
  __ompt_end_team_info_pair_init(&team_info->end_team_info_pair[1]);
  team_info->end_team_info = &team_info->end_team_info_pair[0];

  ompt_task_info_t *task_info = &ompt_pair_ptr->ompt_task_info;
  task_info->task_data.value = 0;
  task_info->frame.enter_frame = ompt_data_none;
  task_info->frame.enter_frame_flags = 0;;
  task_info->frame.exit_frame = ompt_data_none;
  task_info->frame.exit_frame_flags = 0;;
  task_info->scheduling_parent = NULL;
  task_info->lwt_done_sh = 0;
}


void __ompt_lw_taskteam_init(ompt_lw_taskteam_t *lwt, kmp_info_t *thr, int gtid,
                             ompt_data_t *ompt_pid, void *codeptr) {
  lwt->heap = 0;
  lwt->parent = 0;
  // Indicator that lwt is not properly initialized.
  lwt->ompt_info = NULL;
  lwt->td_flags = NULL;

  __ompt_lw_copy_ompt_info_pair(&lwt->ompt_info_pairs[0], ompt_pid, codeptr);
  __ompt_lw_copy_ompt_info_pair(&lwt->ompt_info_pairs[1], ompt_pid, codeptr);

  // FIXME VI3-NOW: Is this ok to do?
  // invalidate all task flags
  memset(&lwt->td_flags_pair[0], 0, sizeof(kmp_tasking_flags_t));
  memset(&lwt->td_flags_pair[1], 0, sizeof(kmp_tasking_flags_t));
}

void __ompt_lw_taskteam_link(ompt_lw_taskteam_t *lwt, kmp_info_t *thr,
                             int on_heap, bool always) {
  ompt_lw_taskteam_t *link_lwt = lwt;
  if (always ||
      thr->th.th_team->t.t_serialized >
          1) { // we already have a team, so link the new team and swap values
    if (on_heap) { // the lw_taskteam cannot stay on stack, allocate it on heap
      link_lwt =
          (ompt_lw_taskteam_t *)__kmp_allocate(sizeof(ompt_lw_taskteam_t));
    }
    link_lwt->heap = on_heap;

    // lwt would be swap in the (on_stack) case, so don't use it after
    // this function ends.

    if (link_lwt != lwt) {
      // The reason to copy information to link_lwt is to be sure that the
      // signal handler will ses those information if the sample is delivered
      // during the linking process.
      link_lwt->ompt_info_pairs[0].ompt_team_info = lwt->ompt_info_pairs[0].ompt_team_info;
      link_lwt->ompt_info_pairs[0].ompt_task_info = lwt->ompt_info_pairs[0].ompt_task_info;
      // NOTE: It may be possible to use only the first element of ompt_info_pair.
      link_lwt->ompt_info_pairs[1].ompt_team_info = lwt->ompt_info_pairs[1].ompt_team_info;
      link_lwt->ompt_info_pairs[1].ompt_task_info = lwt->ompt_info_pairs[1].ompt_task_info;
    }

    kmp_base_team_t *cur_team = &thr->th.th_team->t;
    kmp_taskdata_t *cur_task = thr->th.th_current_task;
    // mark that ompt_team_info is going to be updated inside cur_team
    PTR_MASK_LOWER_BIT(cur_team->ompt_team_info, ompt_team_info_t);
    // mark that ompt_task_info is going to be updated inside cur_task
    PTR_MASK_LOWER_BIT(cur_task->ompt_task_info, ompt_task_info_t);

    // indicator that the link_lwt is not ready yet
    KMP_DEBUG_ASSERT(!link_lwt->ompt_info);
    KMP_DEBUG_ASSERT(!link_lwt->td_flags);

    // set the parent
    link_lwt->parent = thr->th.th_team->t.ompt_serialized_team_info;
    // mark that link_lwt is not fully initialized
    LWT_MASK_STATE(link_lwt, LWT_STATE_LINKING);
    // Update the head of lwt's list, which at she same time serves as an
    // indicator to signal handler that linking process is in progress.
    cur_team->ompt_serialized_team_info = link_lwt;

    // After this point, runtime (this function) and signal handler
    // (__ompt_get_task_info_internal function) may interleave.
    // We will try to avoid potential race conditions.

    // unmask link_lwt in order to use it
    LWT_UNMASK_STATE(link_lwt);
    // The pointers are unmasked in order to easier use them.
    ompt_team_info_t *cur_team_info =
        PTR_GET_UNMASKED(cur_team->ompt_team_info, ompt_team_info_t);
    ompt_task_info_t *cur_task_info =
        PTR_GET_UNMASKED(cur_task->ompt_task_info, ompt_task_info_t);

    // the following information may be eventually copied to
    // cur_task and cur_team.
    ompt_team_info_t tmp_team = link_lwt->ompt_info_pairs[0].ompt_team_info;
    ompt_task_info_t tmp_task = link_lwt->ompt_info_pairs[0].ompt_task_info;

    ompt_info_pair_t *old_ompt_info = link_lwt->ompt_info;
    if (!old_ompt_info) {
      // copy the information from cur_team and cur_task to link_lwt
      // Note that cur_team_info and cur_task_info may be outdated during copying
      // (if signal handler has finished the linking instead of the runtime).
      // In that case, the following CAS will fail for sure, so copied
      // information won't be used at all.
      link_lwt->ompt_info_pairs[0].ompt_team_info = *cur_team_info;
      link_lwt->ompt_info_pairs[0].ompt_task_info = *cur_task_info;
      // If the signal handler hasn't updated the link_lwt yet, then the CAS
      // will succeed. This means that runtime has successfully initialized
      // the link_lwt.
      // FIXME VI3-NOW: Is it ok to use this operation?
      KMP_COMPARE_AND_STORE_PTR(&link_lwt->ompt_info, old_ompt_info,
                                     &link_lwt->ompt_info_pairs[0]);
    }

    // Copy the information to cur_team if needed
    ompt_team_info_t *old_team_info = cur_team->ompt_team_info;
    if (PTR_HAS_MASKED_LOWER_BIT(old_team_info)) {
      // The lower bit is still masked, which means that the signal handler
      // hasn't copied the information yet. Runtime will do that now.
      cur_team->ompt_team_info_pair[0] = tmp_team;
      // If the signal handler hasn't finished with copying the information by
      // this time point, the CAS will succeed as an indicator that the information
      // is successfully copied to cur_team.
      KMP_COMPARE_AND_STORE_PTR(&cur_team->ompt_team_info, old_team_info,
                                     &cur_team->ompt_team_info_pair[0]);
    }

    // Copy the information to cur_task if needed.
    // This works in the similar as copying of ompt_team_info to cur_team.
    ompt_task_info_t *old_task_info = cur_task->ompt_task_info;
    if (PTR_HAS_MASKED_LOWER_BIT(old_task_info)) {
      cur_task->ompt_task_info_pair[0] = tmp_task;
      KMP_COMPARE_AND_STORE_PTR(&cur_task->ompt_task_info, old_task_info,
                                     &cur_task->ompt_task_info_pair[0]);
    }

    // FIXME VI3-NOW: Something simpler may be potentially implemented if we could cast
    //  kmp_tasking_flags_t to uint32_t.
    // cur_task->td_flags may be overridden by signal handler while
    // executing the if-branch, so memoize the value now.
    kmp_tasking_flags_t old_td_flags = cur_task->td_flags;
    kmp_tasking_flags_t *old_td_flags_ptr = link_lwt->td_flags;
    // Similar to copying the ompt_team_info and ompt_task_info.
    if (!old_td_flags_ptr) {
      link_lwt->td_flags_pair[0] = old_td_flags;
      if (KMP_COMPARE_AND_STORE_PTR(&link_lwt->td_flags, old_td_flags_ptr,
                                     &link_lwt->td_flags_pair[0])){
        // Invalidate cur_task->td_flags if the update succeeded.
        // FIXME VI3-NOW: Should we invalidate only tasktype flag or all of them?
        // NOTE: Even though a race condition may appear on these two lines,
        // both runtime and signal handler will write the same value (0),
        // so the final result is the same.
        cur_task->td_flags.tasktype = 0; // this line may be redundant.
        memset(&cur_task->td_flags, 0, sizeof(kmp_tasking_flags_t));
      }
    }

    // Mark that linking process has been successfully finished.
    // NOTE: Race condition may appear here, but both runtime and signal handler
    // will set the lower two bits to 0, so the final result is the same.
    LWT_UNMASK_STATE(cur_team->ompt_serialized_team_info);

    // Since the linking process has officially finished, invalidate
    // the following field which won't be used by signal handler
    link_lwt->ompt_info->ompt_task_info.lwt_done_sh = 0;

  } else {
    // initialize ompt_info if needed
    __ompt_lwt_initialize(lwt);
    // Must not copy parallel_data from lwt or the content
    // potentially stored inside signal handler after the
    // moment of dispatching the ompt_callback_parallel_end may be lost.
    OMPT_CUR_TEAM_INFO(thr)->master_return_address =
        lwt->ompt_info->ompt_team_info.master_return_address;
    // TODO VI3-NOW: Check whether some task information needs to be copied.
  }
  // LWT pointer should be even
  KMP_DEBUG_ASSERT(~ (((uint64_t)link_lwt & 0x1) ^ 0x0));
}

void __ompt_lw_taskteam_unlink(kmp_info_t *thr) {
  ompt_lw_taskteam_t *lwtask = thr->th.th_team->t.ompt_serialized_team_info;
  if (lwtask) {
    ompt_lw_taskteam_t *lwt_parent = lwtask->parent;

    // the following variables should ease the access
    kmp_base_team_t *cur_team = &thr->th.th_team->t;
    kmp_taskdata_t *cur_task = thr->th.th_current_task;
    // store pointers before masking them
    ompt_team_info_t *cur_team_info = cur_team->ompt_team_info;
    ompt_team_info_t *lwt_team_info = &lwtask->ompt_info->ompt_team_info;

    // mark that ompt_team_info and ompt_task_info should be copied
    PTR_MASK_LOWER_BIT(cur_team->ompt_team_info, ompt_team_info_t);
    PTR_MASK_LOWER_BIT(cur_task->ompt_task_info, ompt_task_info_t);

    // ensure that old_parallel_data and old_master_return_address are marked
    // as non-initialized.
    lwt_team_info->end_team_info = NULL;

    // mark that unlinking process is in progress
    LWT_MASK_STATE(cur_team->ompt_serialized_team_info, LWT_STATE_UNLINKING);

    // From this point, runtime (this function) and signal handler
    // (__ompt_get_task_info_internal) may interleave.

    void *old_end_team_info = lwt_team_info->end_team_info;
    if (!old_end_team_info) {
      // Try to atomically copy parallel_data content and master_return_address,
      // if the signal handler hasn't done it yet.
      // Note that cur_team_info may be outdated. If so, the signal handler
      // has finished the unlinking process, so the CAS fails anyway.
      lwt_team_info->end_team_info_pair[0].old_parallel_data.ptr =
          cur_team_info->parallel_data.ptr;
      lwt_team_info->end_team_info_pair[0].old_master_return_address =
          cur_team_info->master_return_address;
      KMP_COMPARE_AND_STORE_PTR(&lwt_team_info->end_team_info,
                                old_end_team_info,
                                &lwt_team_info->end_team_info_pair[0]);

    }
    // copy ompt_team_info
    ompt_team_info_t *old_ompt_team_info = cur_team->ompt_team_info;
    if (PTR_HAS_MASKED_LOWER_BIT(old_ompt_team_info)) {
      // The signal handler hasn't copied the information from lwtask to
      // cur_team, let's do it now.
      ompt_team_info_t *dst = &cur_team->ompt_team_info_pair[0];
      *dst = lwtask->ompt_info->ompt_team_info;
      // cur_team->ompt_team_info_pair[0]->end_team_info points to the
      // memory that belongs to lwt_team_info.
      // Need to calculate whether lwt_team_info->end_team_info points to the
      // first or second element of lwt_team_info->end_team_info_pair.
      // cur_team->ompt_team_info_pair[0]->end_team_info should point to the
      // element of cur_team->ompt_team_info_pair[0]->end_team_info_pair with
      // the same order number
      KMP_DEBUG_ASSERT(dst->end_team_info == lwt_team_info->end_team_info);
      dst->end_team_info =
          &dst->end_team_info_pair[0];
      // Try to mark that the information is successfully copied.
      // The CAS fails if the signal handler has done it in the meantime.
      KMP_COMPARE_AND_STORE_PTR(&cur_team->ompt_team_info, old_ompt_team_info,
                                dst);
    }

    // copy_ompt_task_info on similar way
    ompt_task_info_t *old_ompt_task_info = cur_task->ompt_task_info;
    if (PTR_HAS_MASKED_LOWER_BIT(old_ompt_task_info)) {
      cur_task->ompt_task_info_pair[0] = lwtask->ompt_info->ompt_task_info;
      KMP_COMPARE_AND_STORE_PTR(&cur_task->ompt_task_info, old_ompt_task_info,
                                &cur_task->ompt_task_info_pair[0]);
    }

    // Copy td_flags.
    // Note that there is a race condition. However, both runtime and signal
    // handler write the same value, so the result is deterministic.
    cur_task->td_flags = *lwtask->td_flags;

    // Pop the lwtask from the list of lwts.
    ompt_lw_taskteam_t *old_first_lwt = cur_team->ompt_serialized_team_info;
    if (LWT_STATE_IS_ACTIVE(old_first_lwt)) {
      // FIXME VI3-NOW: This should be regular statement.
      // The signal handler hasn't finished unlinking, so do that now.
      if (!KMP_COMPARE_AND_STORE_PTR(&cur_team->ompt_serialized_team_info,
                                old_first_lwt,
                                lwt_parent)){
        KMP_DEBUG_ASSERT(false);
      }
    }

    // Since unlinking has been officially finished, invalidate the
    // following field which won't be used by signal handler.
    lwtask->ompt_info->ompt_task_info.lwt_done_sh = 0;

    if (lwtask->heap) {
      __kmp_free(lwtask);
      lwtask = NULL;
    }
  } else {
    ompt_team_info_t *cur_team = OMPT_CUR_TEAM_INFO(thr);
    __ompt_end_team_info_init(cur_team);
    // Copy the value of master_return_address in order to be sent to the
    // ompt_callback_parallel_end.
    // By doing this, we don't need to care whether the unlinking happened
    // before or after the loading of the return address inside
    // __kmp_end_serialized function.
    cur_team->end_team_info->old_master_return_address =
        OMPT_CUR_TEAM_INFO(thr)->master_return_address;
  }
}

//----------------------------------------------------------
// task support
//----------------------------------------------------------

// helper function that extends logic of linking/unlinking lwts
static void inline __ompt_lw_taskteam_link_signal_handler(kmp_info_t *thr,
                                                          ompt_lw_taskteam_t *lwt) {
  // If there is ompt_info set, then check lwt_done_sh, otherwise the link
  // hasn't happened yet.
  char lwt_done_sh =
      lwt->ompt_info ? lwt->ompt_info->ompt_task_info.lwt_done_sh : 0;
  switch (lwt_done_sh) {
    case 1:
      // The signal handler has already finished the linking process, so there's
      // no need to do that again.
      return;
    case 0:
      break;
    default:
      KMP_DEBUG_ASSERT(false);
      break;
  }

  // serve to ease the access
  kmp_base_team_t *cur_team = &thr->th.th_team->t;
  kmp_taskdata_t *cur_task = thr->th.th_current_task;
  KMP_DEBUG_ASSERT(LWT_STATE_IS_ACTIVE(cur_team->ompt_serialized_team_info)
                         & LWT_STATE_LINKING);
  // unmask pointers in order to ease the access
  ompt_team_info_t *cur_team_info =
      PTR_GET_UNMASKED(cur_team->ompt_team_info, ompt_team_info_t);
  ompt_task_info_t *cur_task_info =
      PTR_GET_UNMASKED(cur_task->ompt_task_info, ompt_task_info_t);

  // The following information will be eventually copied to
  // cur_team and cur_task.
  ompt_team_info_t tmp_team = lwt->ompt_info_pairs[1].ompt_team_info;
  ompt_task_info_t tmp_task = lwt->ompt_info_pairs[1].ompt_task_info;

  if (!lwt->ompt_info) {
    // copy the information from cur_team and cur_task to lwt
    lwt->ompt_info_pairs[1].ompt_team_info = *cur_team_info;
    lwt->ompt_info_pairs[1].ompt_task_info = *cur_task_info;
    // Mark that the information is copied to lwt, so the runtime doesn't need
    // to do it.
    lwt->ompt_info = &lwt->ompt_info_pairs[1];
  }

  if (PTR_HAS_MASKED_LOWER_BIT(cur_team->ompt_team_info)) {
    // copy the information to cur_team
    cur_team->ompt_team_info_pair[1] = tmp_team;
    // Mark that the information is copied, so that the runtime won't do that.
    cur_team->ompt_team_info = &cur_team->ompt_team_info_pair[1];
  }

  if (PTR_HAS_MASKED_LOWER_BIT(cur_task->ompt_task_info)) {
    // Copy the information to cur_task
    cur_task->ompt_task_info_pair[1] = tmp_task;
    // Mark that the information is copied, so that the runtime won't do that.
    cur_task->ompt_task_info = &cur_task->ompt_task_info_pair[1];
  }

  if (!lwt->td_flags) {
    // Copy the td_flags to lwt
    lwt->td_flags_pair[1] = cur_task->td_flags;
    // Mark that the information is copied, so that the runtime won't do that.
    lwt->td_flags = &lwt->td_flags_pair[1];
    // Invalidate td_flags present in cur_task
    // FIXME VI3-NOW: This is wrong, since it may be executed
  }
  // There might be race condition here, but both runtime and signal handler
  // are writing zeros.
  cur_task->td_flags.tasktype = 0; // this line may be redundant
  memset(&cur_task->td_flags, 0, sizeof(kmp_tasking_flags_t));

  // We will leave the runtime to mark that the linking process
  // has been finished.
  // Mark that the linking process has been sucessfully finished.
  // LWT_UNMASK_STATE(cur_team->ompt_serialized_team_info);

  // The linking process has been finished, even though it's not been
  // marked as finished yet. In order to know that there's no need to
  // execute this function again when ompt_get_task_info is called
  // multiple times while the signal handler processes the same
  // sample, set the following field to true.
  lwt->ompt_info->ompt_task_info.lwt_done_sh = 1;
}

static void inline __ompt_lw_taskteam_unlink_signal_handler(kmp_info_t *thr,
                                                            ompt_lw_taskteam_t *lwt) {
  // If there is ompt_info set, then check lwt_done_sh, otherwise the link
  // hasn't happened yet.
  char lwt_done_sh =
      lwt->ompt_info ? lwt->ompt_info->ompt_task_info.lwt_done_sh : 0;
  switch (lwt_done_sh) {
    case 1:
      // The signal handler has already finished the linking process, so there's
      // no need to do that again.
      return;
    case 0:
      break;
    default:
      // This should never happen.
      KMP_DEBUG_ASSERT(false);
  }

  // serve to ease the accesss
  kmp_base_team_t *cur_team = &thr->th.th_team->t;
  kmp_taskdata_t *cur_task = thr->th.th_current_task;
  KMP_DEBUG_ASSERT(LWT_STATE_IS_ACTIVE(cur_team->ompt_serialized_team_info)
                         & LWT_STATE_UNLINKING);
  ompt_team_info_t *lwt_team_info = &lwt->ompt_info->ompt_team_info;
  // unmask pointer in order to ease the access
  ompt_team_info_t *cur_team_info =
      PTR_GET_UNMASKED(cur_team->ompt_team_info, ompt_team_info_t);

  if (!lwt_team_info->end_team_info) {
    // Copy the parallel_data and master_return_address from cur_team_info
    lwt_team_info->end_team_info_pair[1].old_parallel_data.ptr =
        cur_team_info->parallel_data.ptr;
    lwt_team_info->end_team_info_pair[1].old_master_return_address =
        cur_team_info->master_return_address;
    lwt_team_info->end_team_info = &lwt_team_info->end_team_info_pair[1];
  }

  if (PTR_HAS_MASKED_LOWER_BIT(cur_team->ompt_team_info)) {
    ompt_team_info_t *dst = &cur_team->ompt_team_info_pair[1];
    // Copy the information from lwt to cur_team
    *dst = lwt->ompt_info->ompt_team_info;
    // cur_team->ompt_team_info_pair[1]->end_team_info points to the
    // memory that belongs to lwt_team_info.
    // Need to calculate whether lwt_team_info->end_team_info points to the
    // first or second element of lwt_team_info->end_team_info_pair.
    // cur_team->ompt_team_info_pair[1]->end_team_info should point to the
    // element of cur_team->ompt_team_info_pair[1]->end_team_info_pair with
    // the same order number
    KMP_DEBUG_ASSERT(dst->end_team_info == lwt_team_info->end_team_info);
    dst->end_team_info =
        &dst->end_team_info_pair[0] +
        (lwt_team_info->end_team_info - &lwt_team_info->end_team_info_pair[0]);
    // this calculates the offset from the first element.

    // Mark that the information is copied so that the runtime doesn't need
    // to do that.
    cur_team->ompt_team_info = dst;
  }

  if (PTR_HAS_MASKED_LOWER_BIT(cur_task->ompt_task_info)) {
    // Copy the information from lwt to cur_task
    cur_task->ompt_task_info_pair[1] = lwt->ompt_info->ompt_task_info;
    // Mark that the information is copied so that the runtime won't do that.
    cur_task->ompt_task_info = &cur_task->ompt_task_info_pair[1];
  }

  // Copy the td_flags
  cur_task->td_flags = *lwt->td_flags;

  // Let the runtime mark the unlinking process as finished.
  // Pop the lwt from lwt's list as the indication that the unlinking has been
  // successfully done by the signal handler.
  // cur_team->ompt_serialized_team_info = lwt->parent;

  // The linking process has been finished, even though it's not been
  // marked as finished yet. In order to know that there's no need to
  // execute this function again when ompt_get_task_info is called
  // multiple times while the signal handler processes the same
  // sample, set the following field to true.
  lwt->ompt_info->ompt_task_info.lwt_done_sh = 1;
}

int __ompt_get_task_info_internal(int ancestor_level, int *type,
                                  ompt_data_t **task_data,
                                  ompt_frame_t **task_frame,
                                  ompt_data_t **parallel_data,
                                  int *thread_num) {
  if (__kmp_get_gtid() < 0)
    return 0;

  if (ancestor_level < 0)
    return 0;

  // copied from __ompt_get_scheduling_taskinfo
  ompt_task_info_t *info = NULL;
  ompt_team_info_t *team_info = NULL;
  kmp_info_t *thr = ompt_get_thread();
  int level = ancestor_level;

  if (thr) {
    kmp_taskdata_t *taskdata = thr->th.th_current_task;
    if (taskdata == NULL)
      return 0;
    // NOTE: taskdata->td_team and thr->th.th_team may differ if thread
    //  is forming the new region, or destroying the one that has recently
    //  finished. This will be used later to detect the thread_num for the
    //  ancestor_level=0.
    kmp_team *team = taskdata->td_team, *prev_team = NULL;

    if (team != thr->th.th_team) {
      // One of the following cases:
      // - thread is creating new parallel region
      // - thread is finishing parallel region
      // - thread is creating the outermost serialized parallel region
      // - thread is finishing the outermost serialized parallel region.
      if (ancestor_level == 0) {
        // Inform the tool that the information about the innermost region is
        // not available. However, thread may inquire the information about
        // tasks at higher ancestor levels.
        return 1;
      }
      // If the tool needs the information about the tasks at higher ancestor
      // levels, then decrease ancestor_level variable.
      ancestor_level--;
    }

    if (team == NULL)
      return 0;
    ompt_lw_taskteam_t *lwt = NULL,
                       *next_lwt = LWT_FROM_TEAM(taskdata->td_team);

    int lwt_state = LWT_STATE_IS_ACTIVE(next_lwt);
    if (lwt_state == LWT_STATE_LINKING) {
      // linking
      __ompt_lw_taskteam_link_signal_handler(thr, LWT_GET_UNMASKED(next_lwt));
      if (ancestor_level == 0) {
        // The thread is in the middle of creating a new serialized parallel
        // region. The information is not fully available, so inform the tool,
        // which may inquire the information about the task at higher levels.
        return 1;
      }
      // decrease the ancestor level
      ancestor_level--;
      // Use the innermost lwt at level 1.
      lwt = LWT_GET_UNMASKED(next_lwt);
      next_lwt = NULL;
    } else if (lwt_state == LWT_STATE_UNLINKING) {
      // unlinking
      __ompt_lw_taskteam_unlink_signal_handler(thr, LWT_GET_UNMASKED(next_lwt));
      if (ancestor_level == 0) {
        // The thread is in the middle of destroying the innermost serialized
        // parallel region. The information is not available at this moment,
        // so inform the tool that it may inquire the information about the
        // task at higher levels.
        return 1;
      }
      // decrease the ancestor_level
      ancestor_level--;
      // next_lwt points to the lwt that is going to be popped from the list.
      // The information present at that lwt is invalid, so skip it.
      next_lwt = LWT_GET_UNMASKED(next_lwt)->parent;
    }

    bool tasks_share_lwt = false;

    while (ancestor_level > 0) {
      // Detect whether we need to access to the next lightweight team
      // or to share lwt->ompt_team_info among multiple tasks.
      if (lwt && !tasks_share_lwt) {
        if (lwt->ompt_info->ompt_task_info.scheduling_parent) {
          // lwt is created by linking an explicit task.
          // Access to the scheduling_parent;
          taskdata = lwt->ompt_info->ompt_task_info.scheduling_parent;
          tasks_share_lwt = true;
          // Note that lwt->ompt_team_info is goind to be reused.
          ancestor_level--;
          next_lwt = NULL;
          continue;
        }
        // next lightweight team (if any)
        lwt = lwt->parent;
      }

      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if ((!lwt || tasks_share_lwt) && taskdata) {
        // first try scheduling parent (for explicit task scheduling)
        if (PTR_GET_UNMASKED(taskdata->ompt_task_info,
                             ompt_task_info_t)->scheduling_parent) {
          taskdata = PTR_GET_UNMASKED(taskdata->ompt_task_info,
                                      ompt_task_info_t)->scheduling_parent;
        } else if (next_lwt) {
          lwt = next_lwt;
          next_lwt = NULL;
        } else {
          if (lwt && lwt->parent && tasks_share_lwt) {
            // All tasks that share the lwt->ompt_team_info are exhausted.
            // Access to the next lwt.
            lwt = lwt->parent;
            tasks_share_lwt = false;
          } else {
            // Even though tasks_share_lwt may be true, there are no more lwts.

            // then go for implicit tasks
            taskdata = taskdata->td_parent;
            if (team == NULL)
              return 0;
            prev_team = team;
            team = team->t.t_parent;
            if (taskdata) {
              next_lwt = LWT_FROM_TEAM(taskdata->td_team);
            }
            // Since the new team is accessed, the tasks cannot share lwt.
            tasks_share_lwt = false;
          }
        }
      }
      ancestor_level--;
    }

    if (lwt) {
      // If multiple tasks are reusing the same lwt, then use the
      // taskdata->ompt_task_info and lwt->ompt_team_info.
      info =
          tasks_share_lwt
            ? PTR_GET_UNMASKED(taskdata->ompt_task_info, ompt_task_info_t)
            : &lwt->ompt_info->ompt_task_info;
      team_info = &lwt->ompt_info->ompt_team_info;
      if (type) {
        kmp_tasking_flags_t *td_flags =
            tasks_share_lwt ? &taskdata->td_flags : lwt->td_flags;
        KMP_DEBUG_ASSERT(td_flags)
        // FIXME: Do we need any other flags?
        *type = td_flags->tasktype ? ompt_task_explicit : ompt_task_implicit;
      }
    } else if (taskdata) {
      info = PTR_GET_UNMASKED(taskdata->ompt_task_info, ompt_task_info_t);
      team_info = PTR_GET_UNMASKED(team->t.ompt_team_info, ompt_team_info_t);
      if (type) {
        if (taskdata->td_parent) {
          *type = (taskdata->td_flags.tasktype ? ompt_task_explicit
                                               : ompt_task_implicit) |
                  TASK_TYPE_DETAILS_FORMAT(taskdata);
        } else {
          *type = ompt_task_initial;
        }
      }
    }
    if (task_data) {
      *task_data = info ? &info->task_data : NULL;
    }
    if (task_frame) {
      // OpenMP spec asks for the scheduling task to be returned.
      *task_frame = info ? &info->frame : NULL;
    }
    if (parallel_data) {
      *parallel_data = team_info ? &(team_info->parallel_data) : NULL;
    }
    if (thread_num) {
      if (level == 0) {
        *thread_num = __kmp_get_tid();
      } else if (level == 1 && team != thr->th.th_team) {
        // Two potential cases:
        // 1. Thread is creating the new parallel region. The team has been
        //    initialized and th_team is updated. However, the corresponding
        //    implicit task is still not created, so th_current_task points
        //    to the parent task.
        // 2. Thread is in the middle of the process of finishing the parallel
        //    region. It has just finished with executing the corresponding
        //    implicit task. This means that the th_current_task points to
        //    the parent task.
        // In both cases, the corresponding parallel team is
        // th_team->t.t_parent == taskdata->td_team, so the thread num
        // is stored inside th_team->t.t_master_tid.
        *thread_num = thr->th.th_team->t.t_master_tid;
      } else if (lwt)
        *thread_num = 0; // encounter on nested serialized parallel region
      else if(!prev_team) {
        // Since the prev_team is still NULL, this means that the thread is
        // the explicit nested inside the innermost parallel region or it is
        // executing the corresponding implicit task. Use the __kmp_get_tid to
        // find the thread_num.
        // NOTE: Take care about the order of the if-else branches.
        *thread_num = __kmp_get_tid();
      } else
        *thread_num = prev_team->t.t_master_tid;
      //        *thread_num = team->t.t_master_tid;
    }
    // FIXME: couldn't we check this before trying to provide
    //  parallel_data, task_data, etc?
    return info ? 2 : 0;
  }
  return 0;
}

int __ompt_get_task_memory_internal(void **addr, size_t *size, int blocknum) {
  if (blocknum != 0)
    return 0; // support only a single block

  kmp_info_t *thr = ompt_get_thread();
  if (!thr)
    return 0;

  kmp_taskdata_t *taskdata = thr->th.th_current_task;
  kmp_task_t *task = KMP_TASKDATA_TO_TASK(taskdata);

  if (taskdata->td_flags.tasktype != TASK_EXPLICIT)
    return 0; // support only explicit task

  void *ret_addr;
  int64_t ret_size = taskdata->td_size_alloc - sizeof(kmp_taskdata_t);

  // kmp_task_t->data1 is an optional member
  if (taskdata->td_flags.destructors_thunk)
    ret_addr = &task->data1 + 1;
  else
    ret_addr = &task->part_id + 1;

  ret_size -= (char *)(ret_addr) - (char *)(task);
  if (ret_size < 0)
    return 0;

  *addr = ret_addr;
  *size = (size_t)ret_size;
  return 1;
}

//----------------------------------------------------------
// target region support
//----------------------------------------------------------

int
__ompt_set_frame_enter_internal
(
  void *addr, 
  int flags,
  int state
)
{
  int gtid = __kmp_entry_gtid();
  kmp_info_t *thr = __kmp_threads[gtid];

  ompt_frame_t *ompt_frame = &OMPT_CUR_TASK_INFO(thr)->frame;
  OMPT_FRAME_SET(ompt_frame, enter, addr, flags); 
  int old_state = thr->th.ompt_thread_info.state; 
  thr->th.ompt_thread_info.state = ompt_state_work_parallel;
  return old_state;
}

//----------------------------------------------------------
// team support
//----------------------------------------------------------
void __ompt_team_info_initialize(kmp_team_t *team) {
  if (!team->t.ompt_team_info) {
    // NOTE VI3-NOW: It is possible that the tam may be duplicated.
    // See the __ompt_task_info_initialize function.
    // initialize the ompt_team_info ptr if not
    team->t.ompt_team_info = &team->t.ompt_team_info_pair[0];
  }
  // TODO VI3-NOW: Check whether this may already have been initialized.
}

void __ompt_task_info_initialize(kmp_taskdata_t *taskdata) {
  if (!taskdata->ompt_task_info ||
      (taskdata->ompt_task_info != &taskdata->ompt_task_info_pair[0])) {
    // If ompt_task_info is NULL, it means that the taskdata has been recently
    // allocated.
    // If ompt_task_info doesn't point to the first element in omp_task_info_pair,
    // it means that the taskdata has been created by duplicating another
    // taskdata (so the ompt_task_info points to another taskdata struct).
    // In both cases, ompt_task_info needs to be properly initialized.
    taskdata->ompt_task_info = &taskdata->ompt_task_info_pair[0];
  }
  // TODO VI3-NOW: Check whether this may already have been initialized.
}

void __ompt_lwt_initialize(ompt_lw_taskteam_t *lwt) {
  if (!lwt->ompt_info) {
    // initialized ompt_info ptr if not
    lwt->ompt_info = &lwt->ompt_info_pairs[0];
  }
}


void __ompt_team_assign_id(kmp_team_t *team, ompt_data_t ompt_pid) {
  // before initializing parallel_data, need to initialize ompt_team_info first.
  __ompt_team_info_initialize(team);
  team->t.ompt_team_info->parallel_data = ompt_pid;
}

//----------------------------------------------------------
// misc
//----------------------------------------------------------

static uint64_t __ompt_get_unique_id_internal() {
  static uint64_t thread = 1;
  static THREAD_LOCAL uint64_t ID = 0;
  if (ID == 0) {
    uint64_t new_thread = KMP_TEST_THEN_INC64((kmp_int64 *)&thread);
    ID = new_thread << (sizeof(uint64_t) * 8 - OMPT_THREAD_ID_BITS);
  }
  return ++ID;
}

ompt_sync_region_t __ompt_get_barrier_kind(enum barrier_type bt,
                                           kmp_info_t *thr) {
  if (bt == bs_forkjoin_barrier)
    return ompt_sync_region_barrier_implicit;

  if (bt != bs_plain_barrier)
    return ompt_sync_region_barrier_implementation;

  if (!thr->th.th_ident)
    return ompt_sync_region_barrier;

  kmp_int32 flags = thr->th.th_ident->flags;

  if ((flags & KMP_IDENT_BARRIER_EXPL) != 0)
    return ompt_sync_region_barrier_explicit;

  if ((flags & KMP_IDENT_BARRIER_IMPL) != 0)
    return ompt_sync_region_barrier_implicit;

  return ompt_sync_region_barrier_implementation;
}
