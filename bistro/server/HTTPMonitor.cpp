/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/server/HTTPMonitor.h"

#include <boost/algorithm/string/predicate.hpp>
#include <glog/logging.h>
#include <zlib.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/ConfigLoader.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/runners/TaskRunner.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/utils/Exception.h"
#include <folly/experimental/AutoTimer.h>
#include <folly/json.h>

DEFINE_int32(http_server_port, 8080, "Port to run HTTP server on");
DEFINE_string(http_server_address, "::", "Address to bind to");

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

HTTPMonitor::HTTPMonitor(
    shared_ptr<ConfigLoader> config_loader,
    shared_ptr<NodesLoader> nodes_loader,
    shared_ptr<TaskStatuses> task_statuses,
    shared_ptr<TaskRunner> task_runner,
    shared_ptr<Monitor> monitor)
  : configLoader_(config_loader),
    nodesLoader_(nodes_loader),
    taskStatuses_(task_statuses),
    taskRunner_(task_runner),
    monitor_(monitor),
    server_(
      FLAGS_http_server_port,
      FLAGS_http_server_address,
      bind(&HTTPMonitor::handleRequest, this, placeholders::_1)
    ),
    serverThread_([this](){ server_.run(); }) {
  LOG(INFO) << "Launched HTTP Monitor on port " << FLAGS_http_server_port;
}

string HTTPMonitor::handleRequest(const string& request) {
  LOG(INFO) << "HTTPMonitor request: " << request;
  folly::AutoTimer<> timer("Handled HTTP monitor request");
  dynamic ret = dynamic::object;
  bool zlib_compress = false;
  try {
    dynamic d(parseJson(request));
    if (const auto* prefs = d.get_ptr("prefs")) {
      if (auto* zlib = prefs->get_ptr("zlib_compress")) {
        zlib_compress = zlib->asBool();
      }
      d.erase("prefs");  // Don't interpret this key as a handler.
    }
    // This can throw, so parse out 'zlib_compress' first
    std::shared_ptr<const Config> c = configLoader_->getDataOrThrow();
    for (const auto& pair : d.items()) {
      try {
        ret[pair.first] =
          dynamic::object("data", handleSingle(*c, pair.second));
      } catch (const std::exception& e) {
        ret[pair.first] = dynamic::object("error", e.what());
      }
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "Error handling monitor request: " << e.what();
    ret = e.what();  // zlib_compress would be ignored with "return"
  }
  auto json(folly::toJson(ret));
  if (zlib_compress) {
    string output;
    // Output buffer must be 0.1% large + 12 bytes?
    output.resize(json.size() + (json.size() / 1000) + 12);
    uLongf resultLen = output.size();
    int res = compress(
      (Bytef*)&output[0],
      &resultLen,
      (const Bytef*)json.data(),
      json.size()
    );
    if (res != Z_OK) {
      LOG(ERROR) << "Error when compressing: " << res;
      return "Unable to gzcompress data";
    }
    return output;
  } else {
    return json.toStdString();
  }
}

namespace {

/**
 * Validates the requested jobs against the current jobs.  If no jobs are
 * requested, returns all jobs.
 *
 * handleSingle() cannot call this eagerly, because the "task_logs" handler
 * requires different semantics for the "jobs" field, which would throw here.
 */
vector<const Job*> getRequestedJobs(const Config& c, const dynamic& d) {
  vector<const Job*> jobs;
  if (auto* p = d.get_ptr("jobs")) {
    for (const auto& j : *p) {
      auto it = c.jobs.find(j.asString().toStdString());
      if (it == c.jobs.end()) {
        throw BistroException("Unknown job: ", j.asString());
      }
      jobs.emplace_back(it->second.get());
    }
  } else {
    for (const auto& pair : c.jobs) {
      jobs.emplace_back(pair.second.get());
    }
  }
  return jobs;
}

}

dynamic HTTPMonitor::handleSingle(const Config& c, const dynamic& d) {
  const auto& handler = d["handler"].asString();
  if (handler == "errors") {
    auto errors = monitor_->copyErrors();
    dynamic d = dynamic::object;
    for (const auto& key_and_error : errors) {
      d[key_and_error.first] = key_and_error.second;
    }
    return d;
  }
  if (handler == "refresh_time") {
    return dynamic(monitor_->getLastUpdateTime());
  }
  if (handler == "jobs") {
    return handleJobs(c, getRequestedJobs(c, d));
  }
  if (handler == "sorted_node_names") {
    return handleNodes(c);
  }
  // TODO: deprecate this in favor of running_tasks
  if (handler == "job_node_runtime") {
    return handleTaskRuntime(d);
  }
  if (handler == "running_tasks") {
    return handleRunningTasks(d);
  }
  if (handler == "histogram") {
    return handleHistograms(
      c,
      getRequestedJobs(c, d),
      d.getDefault("num_samples", 0).asInt()
    );
  }
  if (handler == "task_logs") {
    return handleTaskLogs(c, d);
  }
  if (handler == "delete_job") {
    configLoader_->deleteJob(d["job_id"].asString().toStdString());
    return "deleted";
  }
  if (handler == "save_job") {
    configLoader_->saveJob(d["job_id"].asString().toStdString(), d["job"]);
    return "saved";
  }
  if (handler == "forgive_jobs") {
    if (auto* p = d.get_ptr("jobs")) {
      for (const auto& j : *p) {
        taskStatuses_->forgiveJob(j.asString().toStdString());
      }
    }
    return "forgiven";
  }
  if (handler == "kill_task") {
    auto status_filter_str = d.getDefault(
      "status_filter", "force_done_or_failed"
    ).asString().toStdString();
    cpp2::KilledTaskStatusFilter status_filter;
    if (status_filter_str == "force_done_or_failed") {
      status_filter = cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_FAILED;
    } else if (status_filter_str == "force_done_or_incomplete_backoff") {
      status_filter =
        cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE_BACKOFF;
    } else if (status_filter_str == "force_done_or_incomplete") {
      status_filter = cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE;
    } else if (status_filter_str == "none") {
      status_filter = cpp2::KilledTaskStatusFilter::NONE;
    } else {
      throw BistroException("Unknown status_filter: ", status_filter_str);
    }
    // Throws on failure
    taskRunner_->killTask(
      d["job_id"].asString().toStdString(),
      d["node_id"].asString().toStdString(),
      status_filter
    );
    return "killed";
  }
  throw BistroException("Unknown handler: ", handler);
}

/**
 * Input: {
 *   "jobs": ["array", "of", "job", "ids"],  // all jobs if empty or missing
 *   "nodes": ["array", "of", "job", "ids"],  // all nodes if empty or missing
 *   "sort": "asc" or "desc",  // order in which to iterate through log lines
 *   "time": <secs since 1970 GMT; int>,  // start iteration at this time
 *   "line_id": <opaque string, from next_line_id>  // used to page thru lines
 * }
 *
 * Output: {
 *   "next_line_id": <pass as line_id to your next call to get the next page>,
 *   "lines": [["job_id", "node_id", <time>, "content", "line_id"], ...]
 * }
 *
 * Returns sequential log lines ordered by line ID. This ID is an opaque
 * primary key for log lines.  Its first ordering component is guaranteed to
 * be the timestamp.  Lines are filtered to match the specified "jobs" and
 * "nodes".  Unlike other handlers, jobs or nodes don't have to be current.
 *
 * You can request lines starting from a timestamp, or from a line ID
 * (specifying both is an error).  With ascending sort order, the time/line
 * ID you specify will be the minimal one to show up in your results --
 * otherwise, it'll be the maximal.
 *
 * The number of results you get is implementation-dependent, but you are
 * guaranteed to get *some* lines as long as lines are available in your
 * chosen direction.  The absence of results signals that you can stop
 * paging.
 *
 * To get the next page of results, invoke this handler again with "line_id"
 * set to the "next_line_id" from your previous output.  There are no more
 * pages if "next_line_id" is -1.  To get the previous page, keep line page
 * ID (or time) the same, and toggle "sort".
 */
dynamic HTTPMonitor::handleTaskLogs(const Config& c, const dynamic& d) {
  if (d.get_ptr("columns")) {
    throw BistroException("Custom columns are currently not supported.");
  }

  vector<string> job_ids;
  if (auto dyn_job_ids = d.get_ptr("jobs")) {
    job_ids = folly::convertTo<vector<string>>(*dyn_job_ids);
  }
  vector<string> node_ids;
  if (auto dyn_node_ids = d.get_ptr("nodes")) {
    node_ids = folly::convertTo<vector<string>>(*dyn_node_ids);
  }

  bool is_ascending;
  auto asc_or_desc = d.getDefault("sort", "desc").asString();
  if (boost::iequals(asc_or_desc, "asc")) {  // ignore case, like SQL
    is_ascending = true;
  } else if (boost::iequals(asc_or_desc, "desc")) {
    is_ascending = false;
  } else {
    throw BistroException("Unknown sort order: ", asc_or_desc);
  }

  int64_t line_id = LogLine::kNotALineID;
  if (const auto* dyn_time = d.get_ptr("time")) {
    line_id = LogLine::lineIDFromTime(dyn_time->asInt(), is_ascending);
  }
  if (const auto* dyn_line_id = d.get_ptr("line_id")) {
    if (line_id != LogLine::kNotALineID) {
      throw BistroException("Cannot specify both time and line_id");
    }
    line_id = folly::to<int64_t>(dyn_line_id->asString());  // JS has no int64
  }

  auto log = taskRunner_->getJobLogs(
    d["log_type"].asString().toStdString(),
    job_ids,
    node_ids,
    line_id,
    is_ascending,
    // Server-side regex filtering of retrieved lines -- may cause 0 lines
    // to be returned, but nextLineID will still be correct.
    d.getDefault("regex_filter", "").asString().toStdString()
  );

  // Compose the output JSON
  dynamic lines = {};
  for (const LogLine& l : log.lines) {
    lines.push_back({  // No emplace for dynamic???
      l.jobID,
      l.nodeID,
      l.time,
      l.line,
      folly::to<string>(l.lineID)  // JS has no int64
    });
  }
  dynamic res = dynamic::object
    ("next_line_id", folly::to<string>(log.nextLineID))  // JS has no int64
    ("lines", lines);
  return res;
}

dynamic HTTPMonitor::handleJobs(
    const Config& c,
    const vector<const Job*>& jobs) {
  dynamic ret = dynamic::object;
  for (const auto* job : jobs) {
    ret[job->name()] = job->toDynamic(c);
  }
  return ret;
}

dynamic HTTPMonitor::handleNodes(const Config& c) {
  // Returns a dict of level => list of node names
  dynamic ret = dynamic::object;
  auto nodes = nodesLoader_->getDataOrThrow();
  for (const auto& n : *nodes) {
    const auto& level = c.levels.lookup(n->level());
    ret.setDefault(level, dynamic({})).push_back(n->name());
  }
  return ret;
}

namespace {

unordered_set<string> getJobNameSet(const dynamic& d) {
  unordered_set<string> job_names;
  if (auto* p = d.get_ptr("jobs")) {
    for (const auto& j : *p) {
      job_names.emplace(j.asString().toStdString());
    }
  }
  return job_names;
}

}  // anonymous namespace

dynamic HTTPMonitor::handleTaskRuntime(const dynamic& d) {
  const auto& job_names = getJobNameSet(d);

  // Returns a dict of job => node => runtime
  dynamic ret = dynamic::object;
  auto now = time(nullptr);
  auto running_tasks = taskStatuses_->copyRunningTasks();
  for (const auto& pair : running_tasks) {
    const cpp2::RunningTask& rt = pair.second;
    if (job_names.empty() || job_names.count(rt.job)) {
      ret.setDefault(rt.job)[rt.node] = now - rt.invocationID.startTime;
    }
  }
  return ret;
}

dynamic HTTPMonitor::handleRunningTasks(const dynamic& d) {
  const auto& job_names = getJobNameSet(d);

  // Returns a dict of job => node => {
  //   "start_time": since epoch, "worker_shard": "string"
  // }
  dynamic ret = dynamic::object;
  auto running_tasks = taskStatuses_->copyRunningTasks();
  for (const auto& pair : running_tasks) {
    const cpp2::RunningTask& rt = pair.second;
    if (job_names.empty() || job_names.count(rt.job)) {
      ret.setDefault(rt.job)[rt.node] = dynamic::object()
        ("start_time", rt.invocationID.startTime)
        ("worker_shard", rt.workerShard);
    }
  }
  return ret;
}

dynamic HTTPMonitor::handleHistograms(
    const Config& c,
    const vector<const Job*>& jobs,
    int num_samples) {

  const char* encoding =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  auto hists = monitor_->getHistograms(jobs);
  dynamic ret = dynamic::object;
  dynamic samples = dynamic::object;
  for (const auto job : jobs) {
    auto it = hists.find(job->id());
    if (it == hists.end()) {
      continue;
    }
    ret[job->name()] = dynamic::object;
    samples[job->name()] = dynamic::object;
    for (const auto& pair : it->second) {
      auto& by_level = ret[job->name()].setDefault(pair.first);
      auto& samples_by_level = samples[job->name()].setDefault(pair.first);
      for (const auto& values : pair.second) {
        const int bits = static_cast<int>(values.first);
        std::string encoded;
        encoded.append(1, encoding[bits & 63]);
        if (bits >> 6) {
          encoded.append(1, encoding[bits >> 6]);
        }
        by_level[encoded] = values.second.first;
        if (num_samples) {
          const auto& all_samples = values.second.second;
          samples_by_level[encoded] = dynamic(
            all_samples.begin(),
            all_samples.size() <= num_samples
              ? all_samples.end()
              : all_samples.begin() + num_samples
          );
        }
      }
    }
  }
  ret = dynamic::object("histograms", ret);
  if (num_samples) {
    ret["samples"] = samples;
  }
  return ret;
}

void HTTPMonitor::wait() {
  serverThread_.join();
}

}}
