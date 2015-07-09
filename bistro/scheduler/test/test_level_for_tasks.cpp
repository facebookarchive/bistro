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

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/scheduler/Scheduler.h"
#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatusSnapshot.h"
#include "bistro/bistro/utils/hostname.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

string scheduleOne(const dynamic& d) {
  Config config(d);
  config.addJob("job1", dynamic::object("owner", "owner"), nullptr);

  auto nodes_ptr = make_shared<Nodes>();
  NodesLoader::_fetchNodesImpl(config, nodes_ptr.get());
  TaskCatcher catcher;

  EXPECT_TRUE(Scheduler().schedule(
    time(nullptr),
    config,
    nodes_ptr,
    TaskStatusSnapshot(make_shared<NoOpTaskStore>()),
    std::ref(catcher),
    nullptr  // no monitor to collect errors
  ).areTasksRunning_);
  EXPECT_EQ(1, catcher.tasks.size());
  EXPECT_EQ("job1", catcher.tasks[0].first);
  return catcher.tasks[0].second;
}

TEST(TestLevelForTasks, InstanceNodeOnly) {
  EXPECT_EQ(getLocalHostName(), scheduleOne(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {})
      ("node_sources", {dynamic::object("source", "empty")})
    )
  ));
}

TEST(TestLevelForTasks, CanSelectLevel) {
  dynamic d = dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"l1"})
      ("node_sources", {dynamic::object
        ("source", "manual")("prefs", dynamic::object("a_node", {}))
      })
    );
  EXPECT_EQ("a_node", scheduleOne(d));
  d["level_for_tasks"] = "instance";
  EXPECT_EQ(getLocalHostName(), scheduleOne(d));
}
