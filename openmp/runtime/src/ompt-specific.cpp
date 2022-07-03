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
  lwt->ompt_task_info.frame.exit_frame = ompt_data_none;
  lwt->ompt_task_info.scheduling_parent = NULL;
  lwt->heap = 0;
  lwt->parent = 0;
  // clear tasking flags
  TASKING_FLAGS_CLEAR(&lwt->td_flags);
}

// I think that setting tasktype is enough. Other flags are inherited
// from the enclosing task.
#define VI3_KMP_INIT_IMPLICIT_TASK_FLAGS(task) \
    task->td_flags.tasktype = TASK_IMPLICIT;


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

    sigset_t rt_mask;
    // Initialize full set of signals that will be blocked all at the same time
    sigfillset(&rt_mask);
    // Block signals before the critical section begins.
    sigprocmask(SIG_BLOCK, &rt_mask, NULL);

    thr->th.th_current_task->linking = 23;

    // would be swap in the (on_stack) case.
    ompt_team_info_t tmp_team = lwt->ompt_team_info;
    link_lwt->ompt_team_info = *OMPT_CUR_TEAM_INFO(thr);
    *OMPT_CUR_TEAM_INFO(thr) = tmp_team;

    // link the taskteam into the list of taskteams:
    ompt_lw_taskteam_t *my_parent =
        thr->th.th_team->t.ompt_serialized_team_info;
    link_lwt->parent = my_parent;
    thr->th.th_team->t.ompt_serialized_team_info = link_lwt;

    ompt_task_info_t tmp_task = lwt->ompt_task_info;
    link_lwt->ompt_task_info = *OMPT_CUR_TASK_INFO(thr);
    *OMPT_CUR_TASK_INFO(thr) = tmp_task;

    // copy td_flags
    // Note: cur_task may belong to the explicit task, so we need
    //   to preserve the td_flags->tasktype.
    kmp_taskdata_t *cur_task = thr->th.th_current_task;
    link_lwt->td_flags = cur_task->td_flags;
    // linked task isn't executing at the moment
    link_lwt->td_flags.executing = 0;
    // clear td_flags of cur_task
    TASKING_FLAGS_CLEAR(&cur_task->td_flags);
    VI3_KMP_INIT_IMPLICIT_TASK_FLAGS(cur_task);
    // Since cur_task now represents an implicit task of the serialized
    // parallel region, initialize tasking flags (of cur_task) the same way
    // it is done for implicit tasks of regular regions.
    // Otherwise, td_flags may be inherited from previously linked
    // explicit tasks.
    // Calling this function might introduce a significant overhead.
    // Instead, use the previous macro.
    // __kmp_init_implicit_task_flags(cur_task, thr->th.th_team);

    thr->th.th_current_task->linking = 0;

    // Unblock signals after critical section finished.
    sigprocmask(SIG_UNBLOCK, &rt_mask, NULL);

  } else {
    // this is the first serialized team, so we just store the values in the
    // team and drop the taskteam-object
    *OMPT_CUR_TEAM_INFO(thr) = lwt->ompt_team_info;
    *OMPT_CUR_TASK_INFO(thr) = lwt->ompt_task_info;
  }
}

void __ompt_lw_taskteam_unlink(kmp_info_t *thr) {
  ompt_lw_taskteam_t *lwtask = thr->th.th_team->t.ompt_serialized_team_info;
  if (lwtask) {

    sigset_t rt_mask;
    // Initialize full set of signals that will be blocked all at the same time
    sigfillset(&rt_mask);
    // Block signals before the beginning of the critical section.
    sigprocmask(SIG_BLOCK, &rt_mask, NULL);

    thr->th.th_current_task->linking = 33;

    ompt_task_info_t tmp_task = lwtask->ompt_task_info;
    lwtask->ompt_task_info = *OMPT_CUR_TASK_INFO(thr);
    *OMPT_CUR_TASK_INFO(thr) = tmp_task;

    // copy back the td_flags
    thr->th.th_current_task->td_flags = lwtask->td_flags;
    // unlinked task is executing at the moment
    thr->th.th_current_task->td_flags.executing = 1;

    thr->th.th_team->t.ompt_serialized_team_info = lwtask->parent;

    ompt_team_info_t tmp_team = lwtask->ompt_team_info;
    lwtask->ompt_team_info = *OMPT_CUR_TEAM_INFO(thr);
    *OMPT_CUR_TEAM_INFO(thr) = tmp_team;

    thr->th.th_current_task->linking = 0;

    // Unblock signals after critical section finished.
    sigprocmask(SIG_UNBLOCK, &rt_mask, NULL);

    if (lwtask->heap) {
      __kmp_free(lwtask);
      lwtask = NULL;
    }
  }
  //    return lwtask;
}


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

    if (taskdata->linking) {
      // The thread is creating/destroying nested serialized region.
      // Information about the current region is not available
      // at the moment.
      if (ancestor_level == 0 || ancestor_level == 1) {
        return taskdata->linking;
      }
      // decrease the ancestor_level for the innermost region and its parent
      ancestor_level--;
    }

    if (team == NULL)
      return 0;
    ompt_lw_taskteam_t *lwt = NULL,
                       *next_lwt = LWT_FROM_TEAM(taskdata->td_team);

    bool tasks_share_lwt = false;

    while (ancestor_level > 0) {
      // Detect whether we need to access to the next lightweight team
      // or to share lwt->ompt_team_info among multiple tasks.
      if (lwt && !tasks_share_lwt) {
        if (lwt->ompt_task_info.scheduling_parent) {
          // lwt is created by linking an explicit task.
          // Access to the scheduling_parent;
          taskdata = lwt->ompt_task_info.scheduling_parent;
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
        if (taskdata->ompt_task_info.scheduling_parent) {
          taskdata = taskdata->ompt_task_info.scheduling_parent;
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
            } else {
              // All tasks have been exhausted, so there's no task at the
              // requested ancestor_level. Thus, stop iterating and return 0.
              return 0;
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
            ? &taskdata->ompt_task_info
            : &lwt->ompt_task_info;
      team_info = &lwt->ompt_team_info;
      if (type) {
        // FIXME: Do we need any other flags?
        *type = tasks_share_lwt ? (OMPT_GET_TASK_FLAGS(taskdata))
                                : (OMPT_GET_TASK_FLAGS(lwt));
      }
    } else if (taskdata) {
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
