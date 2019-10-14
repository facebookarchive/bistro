/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/test/MockBistro.h"
#include "bistro/bistro/scheduler/SchedulerPolicies.h"

DECLARE_bool(log_performance);
DECLARE_int32(incremental_sleep_ms);
DECLARE_string(instance_node_name);

using namespace facebook::bistro;
using folly::dynamic;

void checkEvents(MockRunner* runner, std::vector<MockRunner::Event> events) {
  EXPECT_EQ(events.size(), runner->events_.size()) << "Extra element: "
    << ::testing::PrintToString(
      (events.size() > runner->events_.size())
        ? events.back() : runner->events_.back()
    );
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i], runner->events_[i]);
  }
  runner->events_.clear();
}

TEST(TestKillDisabled, KillJobOrphans) {
  registerDefaultSchedulerPolicies();

  FLAGS_log_performance = true;  // Shows orphan calculations.
  FLAGS_instance_node_name = "instance";

  dynamic c = dynamic::object
    ("nodes", dynamic::object("levels", dynamic::array()))
    ("kill_orphan_tasks_after_sec", 0)
    ("resources", dynamic::object)
    ("enabled", true);

  Config config(c);
  config.addJob(
      std::make_shared<Job>(config, "j1", dynamic::object("owner", "me")),
      nullptr);
  MockBistro b(config);

  b.bistro_.scheduleOnce(std::chrono::seconds(0));
  checkEvents(b.runner_.get(), {
    std::make_tuple(MockRunner::EventType::RUN, "j1", "instance"),
  });
  // Nothing happens, the scheduler thinks the job is running.
  b.bistro_.scheduleOnce(std::chrono::seconds(1));
  checkEvents(b.runner_.get(), {});

  // Deleting the job should get it killed immediately.
  config.jobs.erase("j1");
  b.configLoader_->setConfig(config);

  b.bistro_.scheduleOnce(std::chrono::seconds(2));
  checkEvents(b.runner_.get(), {
    std::make_tuple(MockRunner::EventType::KILL_TASK, "j1", "instance"),
  });

  config.killOrphanTasksAfter = std::chrono::seconds(1);  // Kill with delay
  b.configLoader_->setConfig(config);

  b.bistro_.scheduleOnce(std::chrono::seconds(3));
  checkEvents(b.runner_.get(), {});
  b.bistro_.scheduleOnce(std::chrono::seconds(4));
  checkEvents(b.runner_.get(), {
    std::make_tuple(MockRunner::EventType::KILL_TASK, "j1", "instance"),
  });

  config.killOrphanTasksAfter = folly::none;  // Don't kill at all
  b.configLoader_->setConfig(config);

  b.bistro_.scheduleOnce(std::chrono::seconds(30));  // Skip far ahead in time
  checkEvents(b.runner_.get(), {});

  // Re-enable killing & bring the job back
  config.killOrphanTasksAfter = std::chrono::seconds(0);
  config.addJob(
      std::make_shared<Job>(config, "j1", dynamic::object("owner", "me")),
      nullptr);
  b.configLoader_->setConfig(config);

  b.bistro_.scheduleOnce(std::chrono::seconds(60));
  checkEvents(b.runner_.get(), {});  // Task isn't orphaned

  // Now see that a bad config does not trigger killing.  Node errors would
  // be handled analogously, so no test for those.
  b.configLoader_->getDataOrThrow();  // Does not throw
  b.configLoader_->setException("Doom");
  EXPECT_THROW(b.configLoader_->getDataOrThrow(), std::runtime_error);
  b.bistro_.scheduleOnce(std::chrono::seconds(90));
  checkEvents(b.runner_.get(), {});
  b.configLoader_->setConfig(config);  // Config's back and still no kill
  b.bistro_.scheduleOnce(std::chrono::seconds(120));
  checkEvents(b.runner_.get(), {});
}

// TODO(7086740): Add a "node orphans" unit test here (P19839388) as soon
// NodesLoader becomes mockable after my nodes refactor.
