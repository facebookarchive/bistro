/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/runners/BenchmarkRunner.h"

#include <folly/Random.h>

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/statuses/TaskStatus.h"

DEFINE_int32(test_task_duration, 5000, "average test task duration (ms)");
DEFINE_double(test_failure_rate, 0, "Probability of test task failures");

namespace facebook { namespace bistro {

using namespace std;
using namespace folly;

namespace {
  // wait to check queue for finished tasks
  constexpr chrono::milliseconds kIdleWait(100);
}

BenchmarkRunner::BenchmarkRunner()
    : distribution_(0, FLAGS_test_task_duration * 2) {
  runInBackgroundLoop([this](){
    const auto cur = std::chrono::system_clock::now();
    SYNCHRONIZED(queue_) {
      while (!queue_.empty() && queue_.top().due_ <= cur) {
        queue_.top().cb_(
          queue_.top().rt_,
          folly::Random::randDouble01() > FLAGS_test_failure_rate
              ? TaskStatus::done()
              : TaskStatus::errorBackoff("testing")
        );
        queue_.pop();
      }
      if (queue_.empty()) {
        return kIdleWait;
      } else {
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
          queue_.top().due_ - cur
        );
        // must not wait longer than kIdleWait in case a short task gets
        // inserted into the priority queue
        return min(wait , kIdleWait);
      }
    }
    return kIdleWait;
  });
}

BenchmarkRunner::~BenchmarkRunner() {
  stopBackgroundThreads();
}

TaskRunnerResponse BenchmarkRunner::runTaskImpl(
  const std::shared_ptr<const Job>& job,
  const std::shared_ptr<const Node>& node,
  cpp2::RunningTask& rt,
  folly::dynamic& job_args,
  std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  cb(rt, TaskStatus::running());
  emplaceTask(std::move(cb), rt, distribution_(generator_));
  return RanTask;
}

}}
