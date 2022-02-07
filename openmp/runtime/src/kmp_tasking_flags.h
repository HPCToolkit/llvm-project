//
// Created by vi3 on 7/9/21.
//

#ifndef KMP_TASKING_FLAGS_H
#define KMP_TASKING_FLAGS_H

// In order to avoid circular reference, declare the following data structure
// here in order to be safely include in both ompt-internal.h and kmp.h.
// Another solution would be to declare it inside ompt-internal.h.
// However, it logically doesn't belong to only OMPT, so it should rather
// remain in separate header file.

// Set all tasking flags to zero, under assumption
// that kmp_tasking_flags struct is exactly 32 bits long.
#define TASKING_FLAGS_CLEAR(td_flags_ptr) *((uint32_t *)td_flags_ptr) = 0;


typedef struct kmp_tasking_flags { /* Total struct must be exactly 32 bits */
  /* Compiler flags */ /* Total compiler flags must be 16 bits */
  unsigned tiedness : 1; /* task is either tied (1) or untied (0) */
  unsigned final : 1; /* task is final(1) so execute immediately */
  unsigned merged_if0 : 1; /* no __kmpc_task_{begin/complete}_if0 calls in if0
                              code path */
  unsigned destructors_thunk : 1; /* set if the compiler creates a thunk to
                                     invoke destructors from the runtime */
  unsigned proxy : 1; /* task is a proxy task (it will be executed outside the
                         context of the RTL) */
  unsigned priority_specified : 1; /* set if the compiler provides priority
                                      setting for the task */
  unsigned detachable : 1; /* 1 == can detach */
  unsigned hidden_helper : 1; /* 1 == hidden helper task */
  unsigned reserved : 8; /* reserved for compiler use */

  /* Library flags */ /* Total library flags must be 16 bits */
  unsigned tasktype : 1; /* task is either explicit(1) or implicit (0) */
  unsigned task_serial : 1; // task is executed immediately (1) or deferred (0)
  unsigned tasking_ser : 1; // all tasks in team are either executed immediately
  // (1) or may be deferred (0)
  unsigned team_serial : 1; // entire team is serial (1) [1 thread] or parallel
  // (0) [>= 2 threads]
  /* If either team_serial or tasking_ser is set, task team may be NULL */
  /* Task State Flags: */
  unsigned started : 1; /* 1==started, 0==not started     */
  unsigned executing : 1; /* 1==executing, 0==not executing */
  unsigned complete : 1; /* 1==complete, 0==not complete   */
  unsigned freed : 1; /* 1==freed, 0==allocated        */
  unsigned native : 1; /* 1==gcc-compiled task, 0==intel */
  unsigned reserved31 : 7; /* reserved for library use */

} kmp_tasking_flags_t;

#endif //KMP_TASKING_FLAGS_H
