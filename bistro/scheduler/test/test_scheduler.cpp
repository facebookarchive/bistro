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

#include <folly/dynamic.h>
#include <folly/json.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/nodes/test/utils.h"
#include "bistro/bistro/runners/NoOpRunner.h"
#include "bistro/bistro/scheduler/Scheduler.h"
#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/scheduler/UnitTestSchedulerPolicy.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"

using namespace facebook::bistro;
using folly::dynamic;

// Future: if we fix the bug that "disabled node" orphans are not created by
// a parent being disabled (even though that would prevent a task from
// starting!), then this test should get a "host_disabled", with an enabled
// node underneath.  However, the better fix is just to make the node
// fetchers propagate disabled state properly.

TEST(TestScheduler, InvokePolicyAndCheckOrphans) {
  Config config(dynamic::object
    ("nodes", dynamic::object
      ("levels", {"host", "db"})
      ("node_order", "lexicographic")
      ("node_sources", {
        dynamic::object
          ("prefs", dynamic::object
            ("host1", {"host1.db2", "host1.db1"})
            ("host2", {"host2.db2", "host2.db1", "host2.db_disabled"})
            ("host2.db_disabled", dynamic::object("disabled", true))
          )
          ("source", "manual"),
      })
    )
    ("resources", dynamic::object
      ("host", dynamic::object
        ("host_concurrency", dynamic::object("default", 3)("limit", 10))
      )
      ("db", dynamic::object
        ("db_concurrency", dynamic::object("default", 1)("limit", 2))
        ("chicken", dynamic::object("default", 3)("limit", 5))
      )
    )
  );
  config.schedulerType = SchedulerType::UnitTest;
  config.addJob(
    "job",
    dynamic::object
      ("owner", "owner")
      ("resources", dynamic::object("db_concurrency", 2)),
    nullptr
  );
  config.addJob(
    "job_disabled",
    dynamic::object
      ("owner", "owner")
      ("enabled", false)
      ("resources", dynamic::object("chicken", 2)),
    nullptr
  );

  NoOpRunner runner(TaskStatus::running());  // Tasks don't finish
  auto nodes_ptr = std::make_shared<Nodes>();
  NodesLoader::_fetchNodesImpl(config, nodes_ptr.get());
  TaskCatcher catcher;
  TaskStatuses statuses(std::make_shared<NoOpTaskStore>());
  statuses.updateForConfig(config);

  // Start a few tasks, all orphans, but some using resources on valid nodes.
  std::vector<cpp2::RunningTask> expected_orphans;
  for (const auto& jn : std::vector<std::pair<std::string, std::string>>{
    {"bad_job", "host2.db1"},  // "unknown job" orphan
    {"job", "bad_node"},  // "unknown node" orphan
    {"job_disabled", "host1.db2"},  // "disabled job" orphan
    {"job", "host2.db_disabled"},  // "disabled node" orphan
  }) {
    auto job = config.jobs.count(jn.first)
      ? config.jobs.at(jn.first)  // Unknown job? Fake an enabled one.
      : std::make_shared<Job>(config, jn.first, dynamic::object("owner", "o"));
    auto node = getNodeVerySlow(*nodes_ptr, jn.second);
    if (!node) {
      node =  // Unknown node? Make an enabled one in the db node-group.
        std::make_shared<Node>(jn.second, config.levels.lookup("db"), true);
    }
    runner.runTask(
      config, job, *node, nullptr,  // no previous status
      [&](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
        expected_orphans.push_back(rt);
        statuses.updateStatus(job->id(), node->id(), rt, std::move(status));
      }
    );
  }
  EXPECT_EQ(4, expected_orphans.size());

  // This folly::Singleton pointer must live until after the schedule() call.
  auto policy_cob_ptr = UnitTestSchedulerPolicy::testPolicyCob();
  *policy_cob_ptr = [&](std::vector<JobWithNodes>& jwns, TaskRunnerCallback) {
    // Serialize our JobWithNodes to folly::dynamic for easy comparison.
    auto resources_to_dynamic_fn = [](const PackedResources& pr) {
      folly::dynamic d{};
      for (auto r : pr) { d.push_back(r); }
      return d;
    };
    folly::dynamic d = dynamic::object;
    for (const auto& jn : jwns) {
      auto& job_d = (d[jn.job()->name()] = dynamic::object);
      auto& nodes_d = (job_d["node_offsets"] = dynamic::object);
      for (const Node* n : jn.nodes) {
        nodes_d[n->name()] = n->offset;
      }
      auto& rsrcs_d = (job_d["resources"] = dynamic::object);
      for (const auto& ngr : jn.nodeGroupResources()) {
        auto& rsrc_d =
          (rsrcs_d[config.levels.lookup(ngr.first)] = dynamic::object);
        rsrc_d["job_needs"] = resources_to_dynamic_fn(ngr.second.job_);
        rsrc_d["nodes_have"] = resources_to_dynamic_fn(*ngr.second.nodes_);
      }
    }
    // The offsets for these nodes will not show up in JobWithNodes, since
    // they are not eligible to run tasks.  However, the offsets are still
    // important to test (and this makes it clear that the "host" packed
    // resources below are correct).
    EXPECT_EQ(0, getNodeVerySlow(*nodes_ptr, "host1")->offset);
    EXPECT_EQ(1, getNodeVerySlow(*nodes_ptr, "host2")->offset);
    EXPECT_EQ(8, getNodeVerySlow(*nodes_ptr, "host2.db_disabled")->offset);
    // Since we specified "node_order": "lexicographic", and there is only
    // one enabled job, the scheduler is deterministic.
    folly::dynamic expected_d =
      dynamic::object("job", dynamic::object  // The only enabled job.
        // Stride of 2, since there are 2 "db" resources
        ("node_offsets", dynamic::object
          ("host1.db1", 0)
          ("host1.db2", 2)
          ("host2.db1", 4)
          ("host2.db2", 6)
        )
        ("resources", dynamic::object
          ("instance", dynamic::object("nodes_have", {})("job_needs", {}))
          ("worker", dynamic::object("nodes_have", {})("job_needs", {}))
          ("host", dynamic::object("nodes_have", {7, 4})("job_needs", {3}))
          // Example interpretation: The last node is "host2.db_disabled",
          // where job2 consumed both "db_concurrency" slots.
          ("db",
            // Don't depend on the order, in which "db" resources got hashed.
            config.resourceNames.lookup("db_concurrency")
                < config.resourceNames.lookup("chicken")
                ? dynamic::object
                  ("nodes_have", {2, 5,  1, 3,  1, 2,  2, 5,  0, 2})
                  ("job_needs", {2, 3})
              : dynamic::object
                  ("nodes_have", {5, 2,  3, 1,  2, 1,  5, 2,  2, 0})
                  ("job_needs", {3, 2})
          )
        )
      );
    EXPECT_EQ(expected_d, d);
    return 0;
  };

  auto res = Scheduler().schedule(
    time(nullptr),
    config,
    nodes_ptr,
    statuses.copySnapshot(),
    std::ref(catcher),  // Ignored, the policy doesn't run anything anyway.
    nullptr  // No monitor to collect errors
  );
  EXPECT_TRUE(res.areTasksRunning_);
  EXPECT_EQ(expected_orphans, res.orphanTasks_);
}
