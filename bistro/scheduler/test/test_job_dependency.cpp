/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/scheduler/Scheduler.h"
#include "bistro/bistro/scheduler/SchedulerPolicies.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/statuses/TaskStore.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

dynamic c = dynamic::object
  ("nodes", dynamic::object
    ("levels", dynamic::array("host", "db"))
    ("node_sources", dynamic::array(dynamic::object
      ("source", "manual")
      ("prefs", dynamic::object
        ("host1", dynamic::array("db11", "db12"))
        ("host2", dynamic::array("db21", "db22"))
      )
    ))
  )
  ("resources", dynamic::object
    ("host", dynamic::object
      ("host_concurrency", dynamic::object
        ("default", 1)
        ("limit", 2)
      )
    )
    ("db", dynamic::object
      ("db_concurrency", dynamic::object
        ("default", 1)
        ("limit", 1)
      )
    )
  )
  ("scheduler", "ranked_priority")
;

// CMake's ctest will run all these tests sequentially.
bool test_registered_scheduler_policies = false;
void testRegisterSchedulerPolicies() {
  if (!test_registered_scheduler_policies) {
    registerDefaultSchedulerPolicies();
    test_registered_scheduler_policies = true;
  }
}

TEST(TestDependencyScheduling, HandleInvalidDependency) {
  testRegisterSchedulerPolicies();
  Config config(c);
  config.addJob(
    std::make_shared<Job>(
        config,
        "job1",
        dynamic::object
          ("owner", "owner")
          ("depends_on", dynamic::array("job2"))),
    nullptr
  );

  auto nodes_ptr = make_shared<Nodes>();
  NodesLoader::_fetchNodesImpl(config, nodes_ptr.get());
  TaskCatcher catcher;

  ASSERT_FALSE(Scheduler().schedule(
    time(nullptr),
    config,
    nodes_ptr,
    TaskStatusSnapshot(make_shared<NoOpTaskStore>()),
    std::ref(catcher),
    nullptr  // no monitor to collect errors
  ).areTasksRunning_);
  ASSERT_TRUE(catcher.tasks.empty());
}

TEST(TestDependencyScheduling, HandleAll) {
  testRegisterSchedulerPolicies();
  Config config(c);
  config.addJob(
    std::make_shared<Job>(
        config,
        "job1",
        dynamic::object
          ("owner", "owner")
          ("priority", 10.0)
          ("depends_on", dynamic::array("job2"))),
    nullptr
  );
  config.addJob(
    std::make_shared<Job>(
        config,
        "job2",
        dynamic::object
          ("owner", "owner")
          ("priority", 1.0)),
    nullptr
  );

  auto nodes_ptr = make_shared<Nodes>();
  NodesLoader::_fetchNodesImpl(config, nodes_ptr.get());
  TaskStatuses task_statuses(make_shared<NoOpTaskStore>());
  task_statuses.updateForConfig(config);
  TaskCatcher catcher;

  ASSERT_TRUE(Scheduler().schedule(
    time(nullptr),
    config,
    nodes_ptr,
    task_statuses.copySnapshot(),
    std::ref(catcher),
    nullptr  // no monitor to collect errors
  ).areTasksRunning_);
  ASSERT_EQ(4, catcher.tasks.size());
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job2", "db11")));
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job2", "db12")));
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job2", "db21")));
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job2", "db22")));

  cpp2::RunningTask rt;
  rt.job = "job2";
  rt.node = "db11";
  task_statuses.updateStatus(rt, TaskStatus::running());
  task_statuses.updateStatus(rt, TaskStatus::done());
  rt.node = "db12";
  task_statuses.updateStatus(rt, TaskStatus::running());

  catcher.tasks.clear();
  ASSERT_TRUE(Scheduler().schedule(
    time(nullptr),
    config,
    nodes_ptr,
    task_statuses.copySnapshot(),
    std::ref(catcher),
    nullptr  // no monitor to collect errors
  ).areTasksRunning_);
  ASSERT_EQ(3, catcher.tasks.size());
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job1", "db11")));
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job2", "db21")));
  EXPECT_TRUE(catcher.contains(make_pair<string, string>("job2", "db22")));
}
