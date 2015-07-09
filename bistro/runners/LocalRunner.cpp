/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/runners/LocalRunner.h"

#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/LogWriter.h"

DEFINE_bool(
  use_soft_kill, true,
  "To kill, send a task SIGTERM, wait, and only then SIGKILL"
);

namespace facebook { namespace bistro {

using namespace std;

LocalRunner::LocalRunner(
    const boost::filesystem::path& cmd,
    const boost::filesystem::path& dir)
  // The default log filename matches BistroRemoteWorkerHandler
  : taskQueue_(dir / "/task_logs.sql3", dir / "/pipes"),
    cmd_(cmd),
    jobsDir_(dir / "/jobs") {
}

TaskRunnerResponse LocalRunner::runTaskImpl(
  const std::shared_ptr<const Job>& job,
  const std::shared_ptr<const Node>& node,
  cpp2::RunningTask& running_task,
  folly::dynamic& job_args,
  function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  // Since it runs locally, the task treats the instance node as the worker.
  auto instance_node = Nodes::getInstanceNodeName();
  job_args["worker_node"] = instance_node;
  job_args["worker_host"] = getLocalHostName();
  // No need to populate resources_by_node, since TaskRunner::runTask would
  // have taken care of the instance node already.

  cb(running_task, TaskStatus::running());
  taskQueue_.runTask(
    running_task,
    {cmd_.native()},
    folly::toJson(job_args).toStdString(),  // Job config argument
    jobsDir_ / running_task.job,  // Working directory for the task
    cb
  );
  return RanTask;
}

LogLines LocalRunner::getJobLogs(
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    const string& regex_filter) {

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
    const std::string& job,
    const std::string& node,
    cpp2::KilledTaskStatusFilter status_filter) {
  taskQueue_.killTask(
    job,
    node,
    FLAGS_use_soft_kill ? cpp2::KillMethod::SOFT : cpp2::KillMethod::HARD,
    status_filter
  );
}

}}
