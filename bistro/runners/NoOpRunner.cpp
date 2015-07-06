#include "bistro/bistro/runners/NoOpRunner.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/statuses/TaskStatus.h"

namespace facebook { namespace bistro {

TaskRunnerResponse NoOpRunner::runTaskImpl(
  const std::shared_ptr<const Job>& job,
  const std::shared_ptr<const Node>& node,
  cpp2::RunningTask& rt,
  folly::dynamic& job_args,
  std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  cb(rt, TaskStatus::running());
  cb(rt, TaskStatus::done());
  return RanTask;
}

}}
