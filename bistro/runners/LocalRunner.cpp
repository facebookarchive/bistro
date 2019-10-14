/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/runners/LocalRunner.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/LogWriter.h"

namespace facebook { namespace bistro {

using namespace std;

LocalRunner::LocalRunner(
    const boost::filesystem::path& cmd,
    const boost::filesystem::path& dir)
  // The default log filename matches BistroRemoteWorkerHandler
  : taskQueue_(std::make_unique<LogWriter>(dir / "/task_logs.sql3")),
    cmd_(cmd),
    jobsDir_(dir / "/jobs") {
}

TaskRunnerResponse LocalRunner::runTaskImpl(
  const std::shared_ptr<const Job>& job,
  const Node&,
  cpp2::RunningTask& running_task,
  folly::dynamic& job_args,
  function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  // From the point of view of cgroup reaping, all tasks of LocalRunner will
  // get lumped into a single shard -- see more details in
  // TaskSubprocessState::makeCGroupProcsPaths.  This is equivalent to
  // saying "there should only be **one** LocalRunner per whatever your
  // current cgroup namespace happens to be.  That's probably not what
  // people want, but until a use case comes up, this is good enough.
  running_task.workerShard = "local";

  // Since it runs locally, the task treats the instance node as the worker.
  auto instance_node = Nodes::getInstanceNodeName();
  job_args["worker_node"] = instance_node;
  job_args["worker_host"] = getLocalHostName();
  // No need to populate resources_by_node, since TaskRunner::runTask would
  // have taken care of the instance node already.

  cb(running_task, TaskStatus::running());
  taskQueue_.runTask(
    running_task,
    job->command().empty()
      ? std::vector<std::string>{cmd_.native()} : job->command(),
    folly::toJson(job_args),  // Job config argument
    jobsDir_ / running_task.job,  // Working directory for the task
    cb,
    [](const cpp2::RunningTask&, cpp2::TaskPhysicalResources&&){},
    job->taskSubprocessOptions()
  );
  return RanTask;
}

LogLines LocalRunner::getJobLogs(
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    const string& regex_filter) const {

  return taskQueue_.getLogWriter()->getJobLogs(
    logtype,
    jobs,
    nodes,
    line_id,
    is_ascending,
    1000,  // log lines to fetch
    regex_filter
  );
}

void LocalRunner::killTask(
    const cpp2::RunningTask& rt,
    const cpp2::KillRequest& req) {
  taskQueue_.kill(rt, req);
}

}}
