/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/Bistro.h"
#include "bistro/bistro/config/InMemoryConfigLoader.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/runners/TaskRunner.h"

// These mocks make it simpler to test whole-Bistro setups.

namespace facebook { namespace bistro {

struct BitBucketTaskStore : public TaskStore {
  void fetchJobTasks(
      const std::vector<std::string>& /*job_ids*/,
      Callback /*cb*/) override {}
  void store(
      const std::string& /*j*/,
      const std::string& /*n*/,
      TaskResult /*r*/) override {}
};

struct MockRunner : public TaskRunner {
  enum class EventType { RUN, KILL_TASK };
  using Event = std::tuple<EventType, std::string, std::string>;

  bool canKill() override { return true; }

  void killTask(
    const cpp2::RunningTask& rt, const cpp2::KillRequest&
  ) override {
    events_.emplace_back(EventType::KILL_TASK, rt.job, rt.node);
  }

  TaskRunnerResponse runTaskImpl(
      const std::shared_ptr<const Job>& job,
      const Node& node,
      cpp2::RunningTask& rt,
      folly::dynamic& /*job_args*/,
      std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)>
          cb) noexcept override {
    cb(rt, TaskStatus::running());  // Otherwise the scheduler won't know.
    events_.emplace_back(EventType::RUN, job->name(), node.name());
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

}}  // namespace facebook::bistro
