/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/server/ThriftMonitor.h"

#include <folly/experimental/AutoTimer.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/ConfigLoader.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/remote/RemoteWorkerState.h"
#include "bistro/bistro/runners/TaskRunner.h"
#include "bistro/bistro/thrift/ThriftConversion.h"
#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;
using apache::thrift::debugString;

ThriftMonitor::ThriftMonitor(
    shared_ptr<ConfigLoader> config_loader,
    shared_ptr<NodesLoader> nodes_loader,
    shared_ptr<TaskStatuses> task_statuses,
    shared_ptr<TaskRunner> task_runner,
    shared_ptr<Monitor> monitor)
    : fb303::FacebookBase2("BistroScheduler"),
      status_(fb303::cpp2::fb_status::STARTING),
      configLoader_(config_loader),
      nodesLoader_(nodes_loader),
      taskStatuses_(task_statuses),
      taskRunner_(task_runner),
      monitor_(monitor) {

  status_ = fb303::cpp2::fb_status::ALIVE;
}

ThriftMonitor::~ThriftMonitor() {
  status_ = fb303::cpp2::fb_status::STOPPING;
}

fb303::cpp2::fb_status ThriftMonitor::getStatus() {
  return status_;
}

void ThriftMonitor::updateStatus(
    const cpp2::RunningTask& rt,
    const std::string& status_str,
    const cpp2::BistroInstanceID& scheduler_id,
    const cpp2::BistroInstanceID& worker_id) {
  try {
    auto status = TaskStatus::fromString(status_str);
    if (status.isRunning()) {
      LOG(ERROR) << "Remote workers are not allowed to send 'running' "
        << "statuses, but we got one: " << status_str;
      return;  // Ignore the status; the worker won't re-send it.
    }
    // If this throws, the remote worker will re-send the status update.
    taskRunner_->remoteUpdateStatus(
      rt, std::move(status), scheduler_id, worker_id
    );
  } catch (const exception& e) {
    LOG(ERROR) << "Error updating worker status: " << e.what();
    throw;  // Have the worker re-send the status later.
  }
}

void ThriftMonitor::processHeartbeat(
    cpp2::SchedulerHeartbeatResponse& out_response,
    const cpp2::BistroWorker& worker,
    const cpp2::WorkerSetID& worker_set_id) {
  out_response =
    taskRunner_->processWorkerHeartbeat(worker, worker_set_id);  // may throw
}

void ThriftMonitor::getJobs(vector<cpp2::BistroJobConfig>& out) {
  std::shared_ptr<const Config> c = configLoader_->getDataOrThrow();
  for (const auto& pair : c->jobs) {
    out.emplace_back(toThrift(pair.second->name(), pair.second->toDynamic(*c)));
  }
}

void ThriftMonitor::getJob(cpp2::BistroJobConfig& r, const string& job) {
  std::shared_ptr<const Config> c = configLoader_->getDataOrThrow();
  auto it = c->jobs.find(job);
  if (it == c->jobs.end()) {
    cpp2::BistroSchedulerUnknownJobException ex;
    ex.message = "Unknown job: " + job;
    throw ex;
  }
  r = toThrift(job, it->second->toDynamic(*c));
}

void ThriftMonitor::deleteJob(const string& job_name) {
  try {
    configLoader_->deleteJob(job_name);
  } catch (const exception& e) {
    cpp2::BistroSchedulerUnknownJobException ex;
    ex.message = e.what();
    throw ex;
  }
}

void ThriftMonitor::saveJob(const cpp2::BistroJobConfig& job) {
  configLoader_->saveJob(job.name, toDynamic(job));
}

void ThriftMonitor::getLevels(vector<string>& levels) {
  const auto l = configLoader_->getDataOrThrow()->levels.all();
  // Remove the first "instance" level and last "worker" level
  copy(l.begin() + 1, l.end() - 1, back_inserter(levels));
}

void ThriftMonitor::getJobHistograms(
    std::vector<cpp2::BistroJobHistogram>& histograms,
    const std::vector<std::string>& job_names,
    int samples) {

  folly::AutoTimer<> timer;
  vector<const Job*> jobs;
  std::shared_ptr<const Config> c = configLoader_->getDataOrThrow();
  if (job_names.empty()) {
    for (const auto& job : c->jobs) {
      jobs.emplace_back(job.second.get());
    }
  } else {
    for (const auto& name : job_names) {
      auto it = c->jobs.find(name);
      // If we don't find this job, we won't return any data. That's more
      // useful than throwing because we might still have data for other jobs.
      if (it != c->jobs.end()) {
        jobs.emplace_back(it->second.get());
      }
    }
  }
  auto hists = monitor_->getHistograms(jobs);
  for (const Job* job : jobs) {
    auto it = hists.find(job->id());
    if (it == hists.end()) {
      continue;
    }
    histograms.emplace_back();
    auto& histogram = histograms.back();
    histogram.job = job->name();
    for (const auto& pair : it->second) {
      auto& s = histogram.statuses[pair.first];
      for (const auto& values : pair.second) {
        const auto key = static_cast<cpp2::BistroTaskStatusBits>(values.first);
        s[key].count = values.second.first;
        if (values.second.second.size() > samples) {
          copy(
            values.second.second.begin(),
            values.second.second.begin() + samples,
            back_inserter(s[key].samples)
          );
        } else {
          s[key].samples = values.second.second;
        }
      }
    }
  }
  timer.log("getJobHistograms finished for ", histograms.size(), " jobs");
}

void ThriftMonitor::getJobLogsByTime(
    cpp2::LogLines& out,
    const string& logtype,
    const string& job_id,
    const string& node_id,
    int32_t time,
    bool is_asc,
    const string& regex_filter) {

  auto line_id = LogLine::lineIDFromTime(time, is_asc);
  getJobLogs(out, logtype, job_id, node_id, line_id, is_asc, regex_filter);
}

void ThriftMonitor::getJobLogsByID(
    cpp2::LogLines& out,
    const string& logtype,
    const string& job_id,
    const string& node_id,
    int64_t line_id,
    bool is_asc,
    const string& regex_filter) {

  getJobLogs(out, logtype, job_id, node_id, line_id, is_asc, regex_filter);
}

void ThriftMonitor::getJobLogs(
    cpp2::LogLines& thrift_out,
    const string& logtype,
    const string& job_id,
    const string& node_id,
    int64_t line_id,
    bool is_ascending,
    const string& regex_filter) {

  LogLines out = taskRunner_->getJobLogs(
    logtype,
    vector<string>{job_id},
    vector<string>{node_id},
    line_id,
    is_ascending,
    regex_filter
  );
  // TODO(#3885590): Get rid of these copies.
  thrift_out.nextLineID = out.nextLineID;
  for (LogLine& l : out.lines) {
    thrift_out.lines.emplace_back(
      apache::thrift::FragileConstructor::FRAGILE,
      std::move(l.jobID),
      std::move(l.nodeID),
      l.time,
      std::move(l.line),
      l.lineID
    );
  }
}

void ThriftMonitor::forgiveJob(const string& job_id) {
  // No need to update the remote worker runner (if any), since we are
  // neither changing runningTasks_ nor unsureIfRunningTasks_.
  taskStatuses_->forgiveJob(job_id);
}

}}
