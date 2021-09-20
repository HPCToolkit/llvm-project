// RUN: %libomp-compile-and-run | FileCheck %s
// REQUIRES: ompt

#include "callback.h"
#include <omp.h>


__attribute__ ((noinline)) // workaround for bug in icc
void print_task_info_at(int ancestor_level, int id)
{
#pragma omp critical
  {
    int task_type;
    char buffer[2048];
    ompt_data_t *parallel_data;
    ompt_data_t *task_data;
    ompt_get_task_info(ancestor_level, &task_type, &task_data, NULL, &parallel_data, NULL);
    format_task_type(task_type, buffer);
    printf("%" PRIu64 ": ancestor_level=%d id=%d task_type=%s=%d "
                      "parallel_id=%" PRIu64 " task_id=%" PRIu64 "\n",
        ompt_get_thread_data()->value, ancestor_level, id, buffer,
        task_type, parallel_data->value, task_data->value);
  }
};

int main()
{
  //initial task
  print_task_info_at(0, 0);

#pragma omp parallel num_threads(1)
  {
    // region 0
    // outermost lwt
    print_task_info_at(0, 1);

#pragma omp task
    {
      // task 0
      print_task_info_at(0, 2);

      // check hierarchy now
      print_task_info_at(0, 2);
      print_task_info_at(1, 1);

#pragma omp parallel num_threads(1)
      {
        // region 1
        print_task_info_at(0, 3);

        // check hierarchy now
        print_task_info_at(0, 3);
        print_task_info_at(1, 2);
        print_task_info_at(2, 1);
        print_task_info_at(0, 3);
      }

      print_task_info_at(0, 2);

    };

    print_task_info_at(0, 1);

  }

  // Check if libomp supports the callbacks for this test.
  // CHECK-NOT: {{^}}0: Could not register callback 'ompt_callback_task_create'
  // CHECK-NOT: {{^}}0: Could not register callback 'ompt_callback_implicit_task'


  // CHECK: {{^}}0: NULL_POINTER=[[NULL:.*$]]
  // CHECK: {{^}}[[MASTER_ID:[0-9]+]]: ompt_event_initial_task_begin: parallel_id={{[0-9]+}}, task_id={{[0-9]+}}, actual_parallelism=1, index=1, flags=1
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=0 task_type=ompt_task_initial=1

  // region 0
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit=[[NULL]], parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_0:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_0]], task_id=[[TASK_ID_0:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]


  // task 0
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_task_create: parent_task_id=[[TASK_ID_0]], parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, new_task_id=[[TASK_ID_1:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=2 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_1]]

  // check hierarchy now
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=2 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=1 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]


  // region 1
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_2:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_1:[0-9]+]], task_id=[[TASK_ID_2:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_2]]


  // check hierarchy now
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_2]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=1 id=2 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=2 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]


  // region 1
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_2]]

  // task 0
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=2 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_1]]

  // region 0
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]

  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_end: parallel_id=[[PARALLEL_ID_0]]

  return 0;
}
