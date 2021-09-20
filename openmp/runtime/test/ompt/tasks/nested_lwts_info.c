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

#pragma omp parallel num_threads(1)
    {
      // region 1
      print_task_info_at(0, 2);

#pragma omp parallel num_threads(1)
      {
        // region 2
        print_task_info_at(0, 3);

#pragma omp task
        {
          // task 0
          print_task_info_at(0, 4);

#pragma omp task
          {
            // task 1
            print_task_info_at(0, 5);

#pragma omp task
            {
              // task 2
              print_task_info_at(0, 6);

              print_task_info_at(0, 6);
              print_task_info_at(1, 5);
              print_task_info_at(2, 4);
              print_task_info_at(3, 3);
              print_task_info_at(4, 2);
              print_task_info_at(5, 1);

#pragma omp parallel num_threads(1)
              {
                // region 3
                print_task_info_at(0, 7);

#pragma omp parallel num_threads(1)
                {
                  // region 4
                  print_task_info_at(0, 8);

#pragma omp parallel num_threads(1)
                  {
                    // region 5
                    print_task_info_at(0, 9);

                    print_task_info_at(0, 9);
                    print_task_info_at(1, 8);
                    print_task_info_at(2, 7);
                    print_task_info_at(3, 6);
                    print_task_info_at(4, 5);
                    print_task_info_at(5, 4);
                    print_task_info_at(6, 3);
                    print_task_info_at(7, 2);
                    print_task_info_at(8, 1);

                    print_task_info_at(0, 9);
                  }

                  print_task_info_at(0, 8);
                }

                print_task_info_at(0, 7);
              }

              print_task_info_at(0, 6);
            };

            print_task_info_at(0, 5);
          };

          print_task_info_at(0, 4);
        };

        print_task_info_at(0, 3);

      }
      print_task_info_at(0, 2);

    }
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

  // region 1
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_1:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_1]], task_id=[[TASK_ID_1:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_1]]

  // region 2
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_2:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_2]], task_id=[[TASK_ID_2:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_2]]

  // task 0
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_task_create: parent_task_id=[[TASK_ID_2]], parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, new_task_id=[[TASK_ID_3:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_3]]

  // task 1
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_task_create: parent_task_id=[[TASK_ID_3]], parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, new_task_id=[[TASK_ID_4:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=5 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_4]]

  // task 2
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_task_create: parent_task_id=[[TASK_ID_4]], parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, new_task_id=[[TASK_ID_5:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=6 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_5]]

  // check hierarchy
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=6 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_5]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=1 id=5 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_4]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=2 id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_3]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=3 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_2]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=4 id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=5 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]


  // region 3
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_3:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_3]], task_id=[[TASK_ID_6:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=7 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_3]] task_id=[[TASK_ID_6]]

  // region 4
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_4:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_4]], task_id=[[TASK_ID_7:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=8 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_4]] task_id=[[TASK_ID_7]]

  // region 5
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_begin: parent_task_id={{[0-9]+}}, parent_task_frame.exit={{0x[0-f]+}}, parent_task_frame.reenter={{0x[0-f]+}}, parallel_id=[[PARALLEL_ID_5:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_implicit_task_begin: parallel_id=[[PARALLEL_ID_5]], task_id=[[TASK_ID_8:[0-9]+]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=9 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_5]] task_id=[[TASK_ID_8]]

  // check hierarchy
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=9 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_5]] task_id=[[TASK_ID_8]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=1 id=8 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_4]] task_id=[[TASK_ID_7]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=2 id=7 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_3]] task_id=[[TASK_ID_6]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=3 id=6 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_5]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=4 id=5 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_4]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=5 id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_3]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=6 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_2]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=7 id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_1]]
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=8 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]

  // region 5
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=9 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_5]] task_id=[[TASK_ID_8]]

  // region 4
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=8 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_4]] task_id=[[TASK_ID_7]]

  // region 3
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=7 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_3]] task_id=[[TASK_ID_6]]

  // task 2
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=6 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_5]]

  // task 1
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=5 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_4]]

  // task 0
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=4 task_type=ompt_task_explicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_3]]

  // region 2
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=3 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_2]] task_id=[[TASK_ID_2]]

  // region 1
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=2 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_1]] task_id=[[TASK_ID_1]]

  // region 0
  // CHECK: {{^}}[[MASTER_ID]]: ancestor_level=0 id=1 task_type=ompt_task_implicit
  // CHECK-SAME: parallel_id=[[PARALLEL_ID_0]] task_id=[[TASK_ID_0]]

  // CHECK: {{^}}[[MASTER_ID]]: ompt_event_parallel_end: parallel_id=[[PARALLEL_ID_0]]

  return 0;
}
