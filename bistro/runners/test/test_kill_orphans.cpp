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

#include "bistro/bistro/Bistro.h"
#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/InMemoryConfigLoader.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/runners/BenchmarkRunner.h"

DECLARE_bool(log_performance);
DECLARE_int32(incremental_sleep_ms);
DECLARE_string(instance_node_name);

using namespace facebook::bistro;
using folly::dynamic;

struct BitBucketTaskStore : public TaskStore {
  void fetchJobTasks(const std::vector<std::string>& job_ids, Callback cb) {}
  void store(const std::string& j, const std::string& n, TaskResult r) {}
};

struct MockRunner : public TaskRunner {
  enum class EventType { RUN, KILL_TASK };
  using Event = std::tuple<EventType, std::string, std::string>;

  bool canKill() override { return true; }

  void killTask(
    const std::string& job,
    const std::string& node,
    cpp2::KilledTaskStatusFilter status_filter
  ) override {
    events_.emplace_back(EventType::KILL_TASK, job, node);
  }

  virtual TaskRunnerResponse runTaskImpl(
    const std::shared_ptr<const Job>& job,
    const std::shared_ptr<const Node>& node,
    cpp2::RunningTask& rt,
    folly::dynamic& job_args,
    std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
  ) noexcept override {
    cb(rt, TaskStatus::running());  // Otherwise the scheduler won't know.
    events_.emplace_back(EventType::RUN, job->name(), node->name());
    return RanTask;
  }

  std::vector<Event> events_;
};

struct MockBistro {
  explicit MockBistro(const Config& config)
    : configLoader_(new InMemoryConfigLoader{config}),
      nodesLoader_(new NodesLoader{
        configLoader_, std::chrono::seconds(3600), std::chrono::seconds(3600)
      }),
      statuses_(new TaskStatuses{std::make_shared<BitBucketTaskStore>()}),
      monitor_(new Monitor{configLoader_, nodesLoader_, statuses_}),
      runner_(new MockRunner),
      bistro_(configLoader_, nodesLoader_, statuses_, runner_, monitor_) {}

  std::shared_ptr<InMemoryConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> statuses_;
  std::shared_ptr<Monitor> monitor_;
  std::shared_ptr<MockRunner> runner_;
  Bistro bistro_;
};

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
  FLAGS_log_performance = true;  // Shows orphan calculations.
  FLAGS_instance_node_name = "instance";
  FLAGS_incremental_sleep_ms = 10;  // Threads exit quickly

  dynamic c = dynamic::object
    ("nodes", dynamic::object("levels", {}))
    ("kill_orphan_tasks_after_sec", 0)
    ("resources", dynamic::object)
    ("enabled", true);

  Config config(c);
  config.addJob("j1", dynamic::object("owner", "me"), nullptr);
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
  config.addJob("j1", dynamic::object("owner", "me"), nullptr);
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
