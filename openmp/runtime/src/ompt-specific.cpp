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

#define OMPT_THREAD_ID_BITS 16

#define PTR_GET_UNMASKED(ptr, type) ptr

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

    while (depth > 0) {
      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (team) {
        team = team->t.t_parent;
      }
      depth--;
    }

    if (team) {
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

    while (depth > 0) {
      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (taskdata) {
        taskdata = taskdata->td_parent;
      }
      depth--;
    }

    if (taskdata) {
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

    while (depth > 0) {
      // next lightweight team (if any)

      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (taskdata) {
        // first try scheduling parent (for explicit task scheduling)
        if (taskdata->ompt_task_info.scheduling_parent) {
          taskdata = taskdata->ompt_task_info.scheduling_parent;
        } else {
          // then go for implicit tasks
          taskdata = taskdata->td_parent;
        }
      }
      depth--;
    }

    if (taskdata) {
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

#define OMPT_GET_TASK_FLAGS(task)                                              \
  (task->td_flags.tasktype ? ompt_task_explicit : ompt_task_implicit) |        \
      TASK_TYPE_DETAILS_FORMAT(task)

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

    while (ancestor_level > 0) {
      // next heavyweight team (if any) after
      // lightweight teams are exhausted
      if (taskdata) {
        // first try scheduling parent (for explicit task scheduling)
        if (taskdata->ompt_task_info.scheduling_parent) {
          taskdata = taskdata->ompt_task_info.scheduling_parent;
        } else {
          // then go for implicit tasks
          taskdata = taskdata->td_parent;
          if (team == NULL)
            return 0;
          prev_team = team;
          team = team->t.t_parent;
          if (!taskdata) {
            return 0;
          }
        }
      } else {
        return 0;
      }
      ancestor_level--;
    }

    if (taskdata) {
      info = &taskdata->ompt_task_info;
      team_info = &team->t.ompt_team_info;
      if (type) {
        if (taskdata->td_parent) {
          *type = OMPT_GET_TASK_FLAGS(taskdata);;
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
      } // encounter on nested serialized parallel region
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
