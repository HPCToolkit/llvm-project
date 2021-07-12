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

#define LWT_MASK(ptr) \
  ptr = (ompt_lw_taskteam_t *)((uint64_t)ptr | 0x1)

#define LWT_UNMASK(ptr) \
  ptr = (ompt_lw_taskteam_t *)((uint64_t)ptr & (0xFFFFFFFFFFFFFFFE))

#define LWT_LINKING_IN_PROGRESS(thr) \
  (uint64_t)thr->th.th_team->t.ompt_serialized_team_info & 0x1

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
      return &lwt->ompt_team_info;
    } else if (team) {
      // extract size from heavyweight team
      if (size)
        *size = team->t.t_nproc;

      // return team info for heavyweight team
      return &team->t.ompt_team_info;
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
      info = &lwt->ompt_task_info;
    } else if (taskdata) {
      info = &taskdata->ompt_task_info;
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
        if (taskdata->ompt_task_info.scheduling_parent) {
          taskdata = taskdata->ompt_task_info.scheduling_parent;
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
      info = &lwt->ompt_task_info;
    } else if (taskdata) {
      info = &taskdata->ompt_task_info;
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

void __ompt_lw_taskteam_init(ompt_lw_taskteam_t *lwt, kmp_info_t *thr, int gtid,
                             ompt_data_t *ompt_pid, void *codeptr) {
  // initialize parallel_data with input, return address to parallel_data on
  // exit
  lwt->ompt_team_info.parallel_data = *ompt_pid;
  lwt->ompt_team_info.master_return_address = codeptr;
  lwt->ompt_task_info.task_data.value = 0;
  lwt->ompt_task_info.frame.enter_frame = ompt_data_none;
  lwt->ompt_task_info.frame.enter_frame_flags = 0;;
  lwt->ompt_task_info.frame.exit_frame = ompt_data_none;
  lwt->ompt_task_info.frame.exit_frame_flags = 0;;
  lwt->ompt_task_info.scheduling_parent = NULL;
  lwt->heap = 0;
  lwt->parent = 0;
  // FIXME VI3-NOW: Is this ok to do?
  // invalidate all task flags
  memset(&lwt->td_flags, 0, sizeof(kmp_tasking_flags_t));
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

    // would be swap in the (on_stack) case.
    ompt_team_info_t tmp_team = lwt->ompt_team_info;
    ompt_task_info_t tmp_task = lwt->ompt_task_info;
    link_lwt->ompt_team_info = *OMPT_CUR_TEAM_INFO(thr);
    link_lwt->ompt_task_info = *OMPT_CUR_TASK_INFO(thr);
    // copy task flags
    link_lwt->td_flags = thr->th.th_current_task->td_flags;
    // link the taskteam into the list of taskteams:
    ompt_lw_taskteam_t *my_parent =
        thr->th.th_team->t.ompt_serialized_team_info;
    link_lwt->parent = my_parent;
    // Mark that link_lwt contains the information from both
    // th_current_task and th_team data structures. They are not safe
    // to be used until the linking process is finished.
    LWT_MASK(link_lwt);
    // link the taskteam into the list of taskteams:
    thr->th.th_team->t.ompt_serialized_team_info = link_lwt;

    *OMPT_CUR_TEAM_INFO(thr) = tmp_team;
    *OMPT_CUR_TASK_INFO(thr) = tmp_task;

    // linking process always happens for an implicit task
    // (this is redundant, if the following statement is used)
    thr->th.th_current_task->td_flags.tasktype = 0;
    // FIXME VI3-NOW: Is it safe to clear other flags?
    memset(&thr->th.th_current_task->td_flags, 0, sizeof(kmp_tasking_flags_t));

    // Mark that linking process has been successfully finished and
    // that both th_current_task and th_team are safe to be used.
    LWT_UNMASK( thr->th.th_team->t.ompt_serialized_team_info);
  } else {
    // Must not copy parallel_data from lwt or the content
    // potentially stored inside signal handler after the
    // moment of dispatching the ompt_callback_parallel_end may be lost.
    OMPT_CUR_TEAM_INFO(thr)->master_return_address =
        lwt->ompt_team_info.master_return_address;
    // TODO VI3-NOW: Check whether some task information needs to be copied.
  }
  // LWT pointer should be even
  KMP_DEBUG_ASSERT(~ (((uint64_t)link_lwt & 0x1) ^ 0x0));
}

void __ompt_lw_taskteam_unlink(kmp_info_t *thr) {
  ompt_lw_taskteam_t *lwtask = thr->th.th_team->t.ompt_serialized_team_info;
  if (lwtask) {
    ompt_lw_taskteam_t *lwt_parent = lwtask->parent;

    ompt_team_info_t tmp_team = lwtask->ompt_team_info;
    ompt_task_info_t tmp_task = lwtask->ompt_task_info;
    // Mark that it is not safe to use neither th_current_task nor th_team
    // until unlinking process is finished.
    LWT_MASK(thr->th.th_team->t.ompt_serialized_team_info);

    // Memoize the content of parallel_data, before invalidating it
    // by unlinikg the lwt. Also, memoize the master_return_address.
    ompt_data_t old_parallel_data = OMPT_CUR_TEAM_INFO(thr)->parallel_data;
    void *old_master_return_address = OMPT_CUR_TEAM_INFO(thr)->master_return_address;

#if 0
    // NOTE: if this is needed, we may need to unmask lwtask
    // This is not used anywhere.
    lwtask->ompt_team_info = *OMPT_CUR_TEAM_INFO(thr);
    lwtask->ompt_task_info = *OMPT_CUR_TASK_INFO(thr);
#endif

    *OMPT_CUR_TEAM_INFO(thr) = tmp_team;
    *OMPT_CUR_TASK_INFO(thr) = tmp_task;
    // Store the content of the old_parallel_data and old_master_return_address
    // in order to be sent to the following ompt_callback_parallel_end.
    OMPT_CUR_TEAM_INFO(thr)->old_parallel_data = old_parallel_data;
    OMPT_CUR_TEAM_INFO(thr)->old_master_return_address = old_master_return_address;
    // copy back the task flags
    thr->th.th_current_task->td_flags = lwtask->td_flags;

    // Finally unlink the lwtask from the list of serialized teams.
    thr->th.th_team->t.ompt_serialized_team_info = lwt_parent;
    // No need to redundantly unmask the lwt_parent, since it has already
    // been unmasked.

    // I think this could be avoided, since lwtask should not be used after this
    // function finishes.
    LWT_UNMASK(lwtask);

    if (lwtask->heap) {
      __kmp_free(lwtask);
      lwtask = NULL;
    }
  } else {
    // Copy the value of master_return_address in order to be sent to the
    // ompt_callback_parallel_end.
    // By doing this, we don't need to care whether the unlinking happened
    // before or after the loading of the return address inside
    // __kmp_end_serialized function.
    OMPT_CUR_TEAM_INFO(thr)->old_master_return_address =
        OMPT_CUR_TEAM_INFO(thr)->master_return_address;
  }
}

//----------------------------------------------------------
// task support
//----------------------------------------------------------

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
    if (team == NULL)
      return 0;
    ompt_lw_taskteam_t *lwt = NULL,
                       *next_lwt = LWT_FROM_TEAM(taskdata->td_team),
                       *prev_lwt = NULL;

    if (LWT_LINKING_IN_PROGRESS(thr)) {
      // Linking is in progress. It is not safe to use neither th_current_task
      // nor th_team. Use the recently linked lwt task that contains the copy
      // of the information present inside th_current_task and th_team right
      // before the linking process has begun.
      lwt = next_lwt;
      LWT_UNMASK(lwt);
      // next_lwt is not needed for this group of nested serialized parallel
      // regions, since we're directly using the lwt for the ancestor_level=0.
      next_lwt = NULL;
    }

    while (ancestor_level > 0) {
      // needed for thread_num
      prev_lwt = lwt;
      // next lightweight team (if any)
      if (lwt)
        lwt = lwt->parent;

      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (!lwt && taskdata) {
        // first try scheduling parent (for explicit task scheduling)
        if (taskdata->ompt_task_info.scheduling_parent) {
          taskdata = taskdata->ompt_task_info.scheduling_parent;
        } else if (next_lwt) {
          lwt = next_lwt;
          next_lwt = NULL;
        } else {
          // then go for implicit tasks
          taskdata = taskdata->td_parent;
          if (team == NULL)
            return 0;
          prev_team = team;
          team = team->t.t_parent;
          if (taskdata) {
            next_lwt = LWT_FROM_TEAM(taskdata->td_team);
          }
          // invalidate prev_lwt, since the regular team has been updated
          prev_lwt = NULL;
        }
      }
      ancestor_level--;
    }

    if (lwt) {
      info = &lwt->ompt_task_info;
      team_info = &lwt->ompt_team_info;
      if (type) {
        *type = ompt_task_implicit;
      }
    } else if (taskdata) {
      info = &taskdata->ompt_task_info;
      team_info = &team->t.ompt_team_info;
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
        if (team != thr->th.th_team) {
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
        } else {
          *thread_num = __kmp_get_tid();
        }
      } else if (prev_lwt)
        *thread_num = 0; // encounter on outermost serialized parallel region
      else if (lwt)
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

void __ompt_team_assign_id(kmp_team_t *team, ompt_data_t ompt_pid) {
  team->t.ompt_team_info.parallel_data = ompt_pid;
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
