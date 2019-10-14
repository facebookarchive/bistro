/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Synchronized.h>
#include <folly/experimental/ThreadedRepeatingFunctionRunner.h>
#include <queue>
#include <chrono>
#include <random>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Job;
class Node;
class TaskStatus;

struct TestTask {
  std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb_;
  cpp2::RunningTask rt_;
  std::chrono::time_point<std::chrono::system_clock> due_;

  TestTask(
      std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb,
      const cpp2::RunningTask& rt,
      std::chrono::milliseconds ms)
    : cb_(std::move(cb)),
      rt_(rt),
      due_(std::chrono::system_clock::now() + ms) {}

  bool operator<(const TestTask& task) const {
    return due_ > task.due_;
  }
};

/**
 * A high-performance runner for local, performance, and stress testing.
 * Its running overhead is almost zero at the turnover rate of 10k tasks/sec.
 * Therefore we can accurately measure the scheduling overhead.
 */
class BenchmarkRunner final : public TaskRunner {
public:
  BenchmarkRunner();
  BenchmarkRunner(const BenchmarkRunner&) = delete;
  ~BenchmarkRunner() override;

  // make this a function for unit test
  inline void emplaceTask(
      std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb,
      const cpp2::RunningTask& rt,
      int ms) {
    ++totalTasks_;
    totalMs_ += ms;
    SYNCHRONIZED(queue_) {
     queue_.emplace(std::move(cb), rt, std::chrono::milliseconds(ms));
     queueMax_ = std::max(queueMax_, queue_.size());
    }
  }

protected:
 TaskRunnerResponse runTaskImpl(
     const std::shared_ptr<const Job>& job,
     const Node& node,
     cpp2::RunningTask& rt,
     folly::dynamic& job_args,
     std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)>
         cb) noexcept override;

private:
  folly::Synchronized<std::priority_queue<TestTask>> queue_;
  // no need to synchronize below because they are used together with queue_
  std::default_random_engine generator_;
  std::uniform_int_distribution<int> distribution_;
  uint64_t totalMs_{0};
  uint64_t totalTasks_{0};
  size_t queueMax_{0};

  // CAUTION: Declared last since the threads access other members of `this`.
  folly::ThreadedRepeatingFunctionRunner backgroundThreads_;
};

}}  // namespace facebook::bistro
