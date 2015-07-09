/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/scheduler/Scheduler.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/statuses/TaskStore.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

dynamic c = dynamic::object
  ("nodes", dynamic::object
    ("levels", {"host", "db"})
    ("node_source", "manual")
      ("node_source_prefs", dynamic::object
        ("host1", {"db11", "db12"})
        ("host2", {"db21", "db22"})
      )
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

TEST(TestDependencyScheduling, HandleInvalidDependency) {
  Config config(c);
  config.addJob(
    "job1",
    dynamic::object
      ("owner", "owner")
      ("depends_on", {"job2"}),
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
  Config config(c);
  config.addJob(
    "job1",
    dynamic::object
      ("owner", "owner")
      ("priority", 10.0)
      ("depends_on", {"job2"}),
    nullptr
  );
  config.addJob(
    "job2",
    dynamic::object
      ("owner", "owner")
      ("priority", 1.0),
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
