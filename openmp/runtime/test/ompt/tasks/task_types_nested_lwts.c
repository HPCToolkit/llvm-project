// RUN: %libomp-compile-and-run | FileCheck %s
// REQUIRES: ompt

#include "callback.h"
#include <omp.h>

__attribute__ ((noinline)) // workaround for bug in icc
void print_task_type_and_parallel_data(int id)
{
  #pragma omp critical
  {
    int task_type;
    char buffer[2048];
    ompt_data_t *parallel_data;
    ompt_get_task_info(0, &task_type, NULL, NULL, &parallel_data, NULL);
    format_task_type(task_type, buffer);
    printf("%" PRIu64 ": id=%d task_type=%s=%d parallel_id=%" PRIu64 "\n",
           ompt_get_thread_data()->value, id, buffer, task_type, parallel_data->value);
  }
};

__attribute__ ((noinline)) // workaround for bug in icc
void print_task_type_and_parallel_data_at_ancestor_level(int id,
                                                         int ancestor_level)
{
#pragma omp critical
  {
    int task_type;
    char buffer[2048];
    ompt_data_t *parallel_data;
    ompt_get_task_info(ancestor_level, &task_type, NULL, NULL, &parallel_data, NULL);
    format_task_type(task_type, buffer);
    printf("%" PRIu64 ": ancestor_level=%d id=%d task_type=%s=%d "
                      "parallel_id=%" PRIu64 "\n",
        ompt_get_thread_data()->value, ancestor_level, id, buffer,
        task_type, parallel_data->value);
  }
};

int main()
{
  //initial task
  print_task_type_and_parallel_data(0);

#pragma omp parallel num_threads(1)
  {
    // region 0
    // outermost lwt
    print_task_type_and_parallel_data(1);

#pragma omp parallel num_threads(1)
    {
      // region 1
      print_task_type_and_parallel_data(2);

#pragma omp task
      {
        // task 0
        print_task_type_and_parallel_data(3);

#pragma omp task
        {
          // task 1
          print_task_type_and_parallel_data(4);

          // check hierarchy now
          print_task_type_and_parallel_data_at_ancestor_level(4, 0);
          print_task_type_and_parallel_data_at_ancestor_level(3, 1);
          print_task_type_and_parallel_data_at_ancestor_level(2, 2);
          print_task_type_and_parallel_data_at_ancestor_level(1, 3);

#pragma omp parallel num_threads(1)
          {
            // region 2
            print_task_type_and_parallel_data(5);

#pragma omp parallel num_threads(1)
            {
              // region 3
              print_task_type_and_parallel_data(6);

              // chech hierarchy now
              print_task_type_and_parallel_data_at_ancestor_level(6, 0);
              print_task_type_and_parallel_data_at_ancestor_level(5, 1);
              print_task_type_and_parallel_data_at_ancestor_level(4, 2);
              print_task_type_and_parallel_data_at_ancestor_level(3, 3);
              print_task_type_and_parallel_data_at_ancestor_level(2, 4);
              print_task_type_and_parallel_data_at_ancestor_level(1, 5);

              print_task_type_and_parallel_data(6);
            }

            print_task_type_and_parallel_data(5);
          }

          print_task_type_and_parallel_data(4);
        };

        print_task_type_and_parallel_data(3);
      };

      print_task_type_and_parallel_data(2);
    }

    print_task_type_and_parallel_data(1);
  }

  // Check if libomp supports the callbacks for this test.
  // CHECK-NOT: {{^}}0: Could not register callback 'ompt_callback_task_create'
  // CHECK-NOT: {{^}}0: Could not register callback 'ompt_callback_implicit_task'


  // CHECK: {{^}}0: NULL_POINTER=[[NULL:.*$]]
  // CHECK: {{^}}[[MASTER_ID:[0-9]+]]: ompt_event_initial_task_begin: parallel_id={{[0-9]+}}, task_id={{[0-9]+}}, actual_parallelism=1, index=1, flags=1
  // CHECK: {{^}}[[MASTER_ID]]: id=0 task_type=ompt_task_initial=1

  // region 0
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit=[[NULL]], parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_0:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_0]]

  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]]

  // region 1
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_1:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_1]]

  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]

  // task 0
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=3 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]

  // task 1
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]

  // check hierarchy now
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=1 id=3 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=2 id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=3 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]]

  // region 2
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_2:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_2]]

  // region 2
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=5 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]]


  // region 3
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_3:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_3]]

  // region 3
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=6 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_3]]

  // check hierarchy now
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=6 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_3]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=1 id=5 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=2 id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=3 id=3 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=4 id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=5 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]]

  // region 3
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=6 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_3]]


  // region 2
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=5 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]]


  // task 1
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]


  // task 0
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=3 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]

  // region 1
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]]

  // region 0
  // we may ignore some task flags
  // CHECK: {{^}}[[MASTER_ID]]: id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]]

  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_end: parallel_id=[[PARALLEL_ID_0]]

  return 0;
}
