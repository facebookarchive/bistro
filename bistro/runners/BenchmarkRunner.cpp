/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/runners/BenchmarkRunner.h"

#include <folly/Random.h>

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/statuses/TaskStatus.h"

DEFINE_int32(test_task_duration, 5000, "average test task duration (ms)");
DEFINE_double(test_failure_rate, 0, "Probability of test task failures");

namespace facebook { namespace bistro {

namespace {
  // wait to check queue for finished tasks
  constexpr std::chrono::milliseconds kIdleWait(100);
}

BenchmarkRunner::BenchmarkRunner()
    : distribution_(0, FLAGS_test_task_duration * 2) {
  // CAUTION: ThreadedRepeatingFunctionRunner recommends two-stage
  // initialization for starting threads.  This specific case is safe since:
  //  - this comes last in the constructor, so the class is fully constructed,
  //  - this class is final, so no derived classes remain to be constructed.
  backgroundThreads_.add("BenchmarkRunner", [this](){
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
  LOG(INFO) << queue_->size() << " still in queue, " << totalMs_ << " ms, "
    << totalTasks_ << " tasks, " << queueMax_ << " max queue size";
  backgroundThreads_.stop();
}

TaskRunnerResponse BenchmarkRunner::runTaskImpl(
    const std::shared_ptr<const Job>&,
    const Node&,
    cpp2::RunningTask& rt,
    folly::dynamic& /*job_args*/,
    std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)>
        cb) noexcept {
  cb(rt, TaskStatus::running());
  emplaceTask(std::move(cb), rt, distribution_(generator_));
  return RanTask;
}

}}  // namespace facebook::bistro
