// RUN: %libomp-compile-and-run | FileCheck %s
// REQUIRES: ompt

#include "callback.h"
#include <omp.h>

#define NO_TASK_ID 0xBAD

__attribute__ ((noinline)) // workaround for bug in icc
void print_task_info_at(int ancestor_level, int id)
{
#pragma omp critical
  {
    int task_type;
    char buffer[2048];
    ompt_data_t *parallel_data;
    ompt_data_t *task_data;
    int thread_num;
    int ret = ompt_get_task_info(ancestor_level, &task_type, &task_data, NULL,
                       &parallel_data, &thread_num);
    if (ret == 2) {
      format_task_type(task_type, buffer);
      printf("%" PRIu64 ": ret=%d ancestor_level=%d id=%d task_type=%s=%d "
                        "parallel_id=%" PRIu64 " task_id=%" PRIu64
                        " thread_num=%d\n",
          ompt_get_thread_data()->value, ret, ancestor_level, id, buffer,
          task_type, parallel_data->value, task_data->value, thread_num);
    } else {
      // Either task doesn't exist at the requested ancestor_level
      // or the information about it is not available.
      printf("%" PRIu64 ": ret=%d\n", ompt_get_thread_data()->value, ret);
    }
  }
};

int main()
{
  //initial task
  print_task_info_at(0, 0);
  // no task at this level
  print_task_info_at(1, NO_TASK_ID);
  // no task at this level
  print_task_info_at(2, NO_TASK_ID);
  // no task at this level
  print_task_info_at(1000, NO_TASK_ID);

#pragma omp parallel num_threads(2)
  {
    // region 0
#pragma omp parallel num_threads(2)
    {
#pragma omp master
#pragma omp task if(0)
      {
#pragma omp task if(0)
        {
          // info about the innermost task
          print_task_info_at(0, 4);
          // info about the outer task
          print_task_info_at(1, 3);
          // info about inner region
          print_task_info_at(2, 2);
          // info about outer region
          print_task_info_at(3, 1);
          // info about initial task
          print_task_info_at(4, 0);
          // no task at higher levels
          print_task_info_at(5, NO_TASK_ID);
          print_task_info_at(6, NO_TASK_ID);
          print_task_info_at(1000, NO_TASK_ID);
        }
      }
    }
  }

  // Check if libomp supports the callbacks for this test.
  // CHECK-NOT: {{^}}0: Could not register callback 'ompt_callback_task_create'
  // CHECK-NOT: {{^}}0: Could not register callback 'ompt_callback_implicit_task'

  // CHECK: {{^}}0: NULL_POINTER=[[NULL:.*$]]
  // CHECK: {{^}}[[MASTER_ID:[0-9]+]]: ompt_event_initial_task_begin
  // CHECK: {{^}}[[MASTER_ID]]: ret=2 ancestor_level=0 id=0 task_type=ompt_task_initial
  // CHECK: {{^}}[[MASTER_ID]]: ret=0
  // CHECK: {{^}}[[MASTER_ID]]: ret=0
  // CHECK: {{^}}[[MASTER_ID]]: ret=0

  // check information inquired from the innermost explicit tasks
  // CHECK: {{^}}[[MASTER_ID]]: ret=2 ancestor_level=0 id=4 task_type=ompt_task_explicit
  // CHECK: {{^}}[[MASTER_ID]]: ret=2 ancestor_level=1 id=3 task_type=ompt_task_explicit
  // CHECK: {{^}}[[MASTER_ID]]: ret=2 ancestor_level=2 id=2 task_type=ompt_task_implicit
  // CHECK: {{^}}[[MASTER_ID]]: ret=2 ancestor_level=3 id=1 task_type=ompt_task_implicit
  // CHECK: {{^}}[[MASTER_ID]]: ret=2 ancestor_level=4 id=0 task_type=ompt_task_initial
  // CHECK: {{^}}[[MASTER_ID]]: ret=0
  // CHECK: {{^}}[[MASTER_ID]]: ret=0
  // CHECK: {{^}}[[MASTER_ID]]: ret=0

  return 0;
}
