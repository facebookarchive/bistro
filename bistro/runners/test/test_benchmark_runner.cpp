/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/runners/BenchmarkRunner.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include <folly/Synchronized.h>

using namespace facebook::bistro;
using namespace folly;
using namespace std;

DECLARE_int32(test_task_duration);
DECLARE_double(test_failure_rate);

TEST(TestBenchmarkRunner, HandleTaskResult) {
  FLAGS_test_task_duration = 10; // 10ms
  BenchmarkRunner runner;
  Synchronized<TaskStatus> status;
  Config c(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1" , "level2"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "range_label")
        ("prefs", dynamic::object)
      ))
    )
    ("resources", dynamic::object)
  );
  dynamic job_d = dynamic::object
    ("enabled", true)
    ("owner", "owner");
  auto job_ptr = make_shared<Job>(c, "invalid_job", job_d);
  Node node("instance_node", 0, true);

  FLAGS_test_failure_rate = 0; // no failure
  runner.runTask(
    c,
    job_ptr,
    node,
    nullptr,  // no previous status
    [&status](const cpp2::RunningTask& rt, TaskStatus&& st) {
      status->update(rt, std::move(st));
    }
  );
  ASSERT_TRUE(status->isRunning());
  this_thread::sleep_for(std::chrono::milliseconds(110)); // sleep an idleWait cycle
  ASSERT_TRUE(status->isDone());

  FLAGS_test_failure_rate = 1; // 100% failure
  status = TaskStatus();
  runner.runTask(
    c,
    job_ptr,
    node,
    nullptr,  // no previous status
    [&status](const cpp2::RunningTask& rt, TaskStatus&& st) {
      status->update(rt, std::move(st));
    }
  );
  ASSERT_TRUE(status->isRunning());
  this_thread::sleep_for(std::chrono::milliseconds(110)); // sleep an idleWait cycle
  ASSERT_TRUE(status->isInBackoff(0));
}

TEST(TestBenchmarkRunner, HandleTaskOrder) {
  BenchmarkRunner runner;
  Synchronized<vector<int>> catcher;
  cpp2::RunningTask rt;
  runner.emplaceTask(
      [&catcher](const cpp2::RunningTask& /*rt*/, TaskStatus&& /*st*/) {
        catcher->push_back(1);
      },
      rt,
      20); // Task 1 finishes in 20ms
  runner.emplaceTask(
      [&catcher](const cpp2::RunningTask& /*rt*/, TaskStatus&& /*st*/) {
        catcher->push_back(2);
      },
      rt,
      10); // Task 2 finishes in 10ms
  this_thread::sleep_for(std::chrono::milliseconds(110)); // sleep an idleWait cycle
  EXPECT_EQ(2, catcher->at(0));
  EXPECT_EQ(1, catcher->at(1));
}
