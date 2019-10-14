/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
#include "bistro/bistro/scheduler/SchedulerPolicies.h"
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

// CMake's ctest will run all these tests sequentially.
bool test_registered_scheduler_policies = false;
void testRegisterSchedulerPolicies() {
  if (!test_registered_scheduler_policies) {
    registerDefaultSchedulerPolicies();
    test_registered_scheduler_policies = true;
  }
}

dynamic jobWithNodesToDynamic(
    const Config& config,
    std::vector<JobWithNodes>& jwns) {
  auto resources_to_dynamic_fn = [](const PackedResources& pr) {
    dynamic d = dynamic::array;
    for (auto r : pr) { d.push_back(r); }
    return d;
  };

  dynamic d = dynamic::object;
  for (const auto& jn : jwns) {
    auto& job_d = (d[jn.job()->name()] = dynamic::object);
    auto& nodes_d = (job_d["node_offsets"] = dynamic::object);
    for (const Node* n : jn.nodes) {
      // Replicas are a hack: multiple nodes with the same name and
      // different parents.  Ensure their offsets are all the same.
      if (auto* existing_offset_ptr = nodes_d.get_ptr(n->name())) {
        EXPECT_EQ(n->offset, existing_offset_ptr->asInt());
      } else {
        nodes_d[n->name()] = n->offset;
      }
    }
    auto& rsrcs_d = (job_d["resources"] = dynamic::object);
    for (const auto& ngr : jn.nodeGroupResources()) {
      auto& rsrc_d =
        (rsrcs_d[config.levels.lookup(ngr.first)] = dynamic::object);
      rsrc_d["job_needs"] = resources_to_dynamic_fn(ngr.second.job_);
      rsrc_d["nodes_have"] = resources_to_dynamic_fn(*ngr.second.nodes_);
    }
  }

  return d;
}

TEST(TestScheduler, InvokePolicyAndCheckOrphans) {
  testRegisterSchedulerPolicies();
  Config config(dynamic::object
    ("nodes", dynamic::object
      ("levels", dynamic::array("host", "db"))
      ("node_order", "lexicographic")
      ("node_sources", dynamic::array(dynamic::object
          ("prefs", dynamic::object
            ("host1", dynamic::array("host1.db2", "host1.db1"))
            ("host2",
              dynamic::array("host2.db2", "host2.db1", "host2.db_disabled"))
            ("host2.db_disabled", dynamic::object("disabled", true))
          )
          ("source", "manual")
      ))
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
  config.schedulerPolicyName = kSchedulePolicyUnitTest.str();
  config.addJob(
    std::make_shared<Job>(
        config,
        "job",
        dynamic::object
          ("owner", "owner")
          ("resources", dynamic::object("db_concurrency", 2))),
    nullptr
  );
  config.addJob(
    std::make_shared<Job>(
        config,
        "job_disabled",
        dynamic::object
          ("owner", "owner")
          ("enabled", false)
          ("resources", dynamic::object("chicken", 2))),
    nullptr
  );

  NoOpRunner runner(TaskStatus::running());  // Tasks don't finish
  auto nodes_ptr = std::make_shared<Nodes>();
  NodesLoader::_fetchNodesImpl(config, nodes_ptr.get());
  TaskCatcher catcher;
  TaskStatuses statuses(std::make_shared<NoOpTaskStore>());
  statuses.updateForConfig(config);

  // Start a few tasks, all orphans, but some using resources on valid nodes.
  std::set<cpp2::RunningTask> expected_orphans;
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
        expected_orphans.insert(rt);
        statuses.updateStatus(job->id(), node->id(), rt, std::move(status));
      }
    );
  }
  EXPECT_EQ(4, expected_orphans.size());

  // This folly::Singleton pointer must live until after the schedule() call.
  auto policy_cob_ptr = UnitTestSchedulerPolicy::testPolicyCob();
  *policy_cob_ptr = [&](std::vector<JobWithNodes>& jwns, TaskRunnerCallback) {
    auto d = jobWithNodesToDynamic(config, jwns);
    // The offsets for these nodes will not show up in JobWithNodes, since
    // they are not eligible to run tasks.  However, the offsets are still
    // important to test (and this makes it clear that the "host" packed
    // resources below are correct).
    EXPECT_EQ(0, getNodeVerySlow(*nodes_ptr, "host1")->offset);
    EXPECT_EQ(1, getNodeVerySlow(*nodes_ptr, "host2")->offset);
    EXPECT_EQ(8, getNodeVerySlow(*nodes_ptr, "host2.db_disabled")->offset);
    // Since we specified "node_order": "lexicographic", and there is only
    // one enabled job, the scheduler is deterministic.
    dynamic expected_d =
      dynamic::object("job", dynamic::object  // The only enabled job.
        // Stride of 2, since there are 2 "db" resources
        ("node_offsets", dynamic::object
          ("host1.db1", 0)
          ("host1.db2", 2)
          ("host2.db1", 4)
          ("host2.db2", 6)
        )
        ("resources", dynamic::object
          ("instance", dynamic::object
            ("nodes_have", dynamic::array)("job_needs", dynamic::array)
          )
          ("worker", dynamic::object
            ("nodes_have", dynamic::array)("job_needs", dynamic::array)
          )
          ("host", dynamic::object
            ("nodes_have", dynamic::array(7, 4))
            ("job_needs", dynamic::array(3))
          )
          // Example interpretation: The last node is "host2.db_disabled",
          // where job2 consumed both "db_concurrency" slots.
          ("db",
            // Don't depend on the order, in which "db" resources got hashed.
            config.resourceNames.lookup("db_concurrency")
                < config.resourceNames.lookup("chicken")
                ? dynamic::object
                  ("nodes_have",
                    dynamic::array(2, 5,  1, 3,  1, 2,  2, 5,  0, 2))
                  ("job_needs", dynamic::array(2, 3))
              : dynamic::object
                  ("nodes_have",
                    dynamic::array(5, 2,  3, 1,  2, 1,  5, 2,  2, 0))
                  ("job_needs", dynamic::array(3, 2))
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
  std::set<cpp2::RunningTask> actual_orphans(
    res.orphanTasks_.begin(), res.orphanTasks_.end()
  );
  EXPECT_EQ(expected_orphans, actual_orphans);
}

struct ReplicaTest {
  ReplicaTest()
    : config_(dynamic::object
        ("nodes", dynamic::object
          ("levels", dynamic::array("host", "db"))
          ("node_order", "lexicographic")
          ("node_sources", dynamic::array(
            dynamic::object
              ("prefs", dynamic::object
                ("host1", dynamic::array("db"))
                ("host2", dynamic::array("db"))
              )
              ("source", "manual")
          ))
        )
        ("resources", dynamic::object
          ("db", dynamic::object
            ("db_concurrency", dynamic::object("default", 1)("limit", 1))
          )
        )
      ),
      runner_(TaskStatus::running()),  // Tasks don't finish
      nodesPtr_(std::make_shared<Nodes>()),
      statuses_(std::make_shared<NoOpTaskStore>()) {

    config_.schedulerPolicyName = kSchedulePolicyUnitTest.str();
    config_.addJob(
        std::make_shared<Job>(
            config_,
            "job",
            dynamic::object("owner", "owner")),
        nullptr);
    NodesLoader::_fetchNodesImpl(config_, nodesPtr_.get());
    statuses_.updateForConfig(config_);
  }

  Scheduler::Result checkSchedule(int db_concurrency_left) {
    // This folly::Singleton pointer must live until after the schedule() call.
    auto policy_cob_ptr = UnitTestSchedulerPolicy::testPolicyCob();
    *policy_cob_ptr = [&](std::vector<JobWithNodes>& jwns, TaskRunnerCallback) {
      auto d = jobWithNodesToDynamic(config_, jwns);
      // If a task is already running on any "db" node, the isRunning()
      // check will exclude it from the JobNodes structure.
      dynamic node_offsets = dynamic::object;
      if (db_concurrency_left == 1) {
        node_offsets["db"] = 0;
      }
      // Since we specified "node_order": "lexicographic", and there is only
      // one enabled job, the scheduler is deterministic.
      dynamic expected_d =
        dynamic::object("job", dynamic::object  // The only enabled job.
          ("node_offsets", node_offsets)
          ("resources", dynamic::object
            ("instance", dynamic::object
              ("nodes_have", dynamic::array)
              ("job_needs", dynamic::array)
            )
            ("worker", dynamic::object
              ("nodes_have", dynamic::array)
              ("job_needs", dynamic::array)
            )
            ("host", dynamic::object
              ("nodes_have", dynamic::array)
              ("job_needs", dynamic::array)
            )
            // Example interpretation: The last node is "host2.db_disabled",
            // where job2 consumed both "db_concurrency" slots.
            ("db", dynamic::object
              ("nodes_have", dynamic::array(db_concurrency_left))
              ("job_needs", dynamic::array(1))
            )
          )
        );
      EXPECT_EQ(expected_d, d);
      return 0;
    };

    return Scheduler().schedule(
      time(nullptr),
      config_,
      nodesPtr_,
      statuses_.copySnapshot(),
      std::ref(catcher_),  // Ignored, the policy doesn't run anything anyway.
      nullptr  // No monitor to collect errors
    );
  }

  Config config_;
  NoOpRunner runner_;
  std::shared_ptr<Nodes> nodesPtr_;
  TaskCatcher catcher_;
  TaskStatuses statuses_;
};

TEST(TestScheduler, EnsureReplicasSharePackedResources) {
  testRegisterSchedulerPolicies();
  auto res = ReplicaTest().checkSchedule(1);
  EXPECT_FALSE(res.areTasksRunning_);
  EXPECT_EQ(0, res.orphanTasks_.size());
}

TEST(TestScheduler, EnsureBothReplicasCanRun) {
  testRegisterSchedulerPolicies();
  // There are two copies of the "db" node, try running on both.
  for (size_t which_copy = 1; which_copy <= 2; ++which_copy) {
    size_t seen_copies = 0;
    // Re-make the test so we don't have to stop the task :D
    ReplicaTest t;
    for (const auto& node : *t.nodesPtr_) {
      if (node->name() != "db") { continue; }
      ++seen_copies;
      if (seen_copies != which_copy) { continue; }

      bool callback_ran = false;
      auto job = t.config_.jobs.at("job");
      t.runner_.runTask(
        t.config_, job, *node,
        nullptr, // no previous status
        [&](const cpp2::RunningTask& rt, TaskStatus&& s) noexcept {
          t.statuses_.updateStatus(job->id(), node->id(), rt, std::move(s));
          callback_ran = true;
        }
      );
      auto res = t.checkSchedule(0);
      EXPECT_TRUE(callback_ran);
      EXPECT_TRUE(res.areTasksRunning_);
      EXPECT_EQ(0, res.orphanTasks_.size());
    }
  }
}
