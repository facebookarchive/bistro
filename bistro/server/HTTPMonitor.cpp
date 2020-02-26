/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/server/HTTPMonitor.h"

#include <boost/algorithm/string/predicate.hpp>
#include <folly/experimental/AutoTimer.h>
#include <folly/json.h>
#include <glog/logging.h>

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
    monitor_(monitor) {
}

fbstring HTTPMonitor::handleRequest(const fbstring& request, bool isSecure) {
  LOG(INFO) << "HTTPMonitor request: " << request;
  folly::AutoTimer<> timer(
    isSecure
      ? "Handled HTTPS monitor request"
      : "Handled HTTP monitor request"
  );
  dynamic ret = dynamic::object;
  try {
    dynamic d(parseJson(request));
    if (const auto* prefs = d.get_ptr("prefs")) {
      d.erase("prefs");  // Don't interpret this key as a handler.
    }
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
    ret = e.what();
  }
  return folly::toJson(ret);
}

namespace {

/**
 * Validates the requested jobs against the current jobs.  If no jobs are
 * requested, returns all jobs. Ignores unknown jobs.
 *
 * handleSingle() cannot call this eagerly, because the "task_logs" handler
 * requires different semantics for the "jobs" field, which would throw here.
 */
vector<const Job*> getRequestedJobs(const Config& c, const dynamic& d) {
  vector<const Job*> jobs;
  auto* p = d.get_ptr("jobs");
  if (p && !p->empty()) {
    for (const auto& j : *p) {
      auto it = c.jobs.find(j.asString());
      if (it != c.jobs.end()) {
        jobs.emplace_back(it->second.get());
      }
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
    dynamic dObj = dynamic::object;
    for (const auto& key_and_error : errors) {
      dObj[key_and_error.first] = key_and_error.second;
    }
    return dObj;
  }
  if (handler == "refresh_time") {
    return dynamic(monitor_->getLastUpdateTime());
  }
  if (handler == "jobs") {
    return handleJobs(c, getRequestedJobs(c, d));
  }
  if (handler == "sorted_node_names") {
    return handleSortedNodeNames(c);
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
    configLoader_->deleteJob(d["job_id"].asString());
    return "deleted";
  }
  if (handler == "save_job") {
    configLoader_->saveJob(d["job_id"].asString(), d["job"]);
    return "saved";
  }
  if (handler == "forgive_jobs") {
    if (auto* p = d.get_ptr("jobs")) {
      for (const auto& j : *p) {
        taskStatuses_->forgiveJob(j.asString());
      }
    }
    return "forgiven";
  }
  if (handler == "nodes") {
    return handleNodes(c, d);
  }
  if (handler == "kill_task") {
    // Ignore d["status_filter"], since that option is now deprecated.
    auto job = d["job_id"].asString();
    auto node = d["node_id"].asString();
    // Look up the job
    auto jit = c.jobs.find(job);
    if (jit == c.jobs.end()) {
      throw BistroException("Unknown job ", job);
    }
    // Look up the running task
    const Job::ID job_id(as_const(Job::JobNameTable)->lookup(job));
    const Node::ID node_id(as_const(Node::NodeNameTable)->lookup(node));
    auto maybe_rt = taskStatuses_->copyRunningTask(job_id, node_id);
    if (!maybe_rt.has_value()) {
      throw BistroException("Unknown running task ", job, ", ", node);
    }
    taskRunner_->killTask(*maybe_rt, jit->second->killRequest());
    return "signaled";
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
dynamic HTTPMonitor::handleTaskLogs(const Config& /*c*/, const dynamic& d) {
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
    d["log_type"].asString(),
    job_ids,
    node_ids,
    line_id,
    is_ascending,
    // Server-side regex filtering of retrieved lines -- may cause 0 lines
    // to be returned, but nextLineID will still be correct.
    d.getDefault("regex_filter", "").asString()
  );

  // Compose the output JSON
  dynamic lines = dynamic::array;
  for (const LogLine& l : log.lines) {
    lines.push_back(dynamic::array(  // No emplace for dynamic???
      l.jobID,
      l.nodeID,
      l.time,
      l.line,
      folly::to<string>(l.lineID)  // JS has no int64
    ));
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

/**
 * Input: {
 *   "nodes": ["array", "of", "node", "ids"],  // all nodes if empty or missing
 *   "fields": ["resources", "disabled"],  // all resources if empty or missing
 * }
 * Output: {
 *   "results" : {
 *     "level_name_0" : {...},
 *     "level_name_1" : {
 *       "node1": {
 *         "resources" : {
 *           "r1" : {"default": w, "limit": x, "weight": y, "override": z},
 *           ...
 *         }
 *         "disabled" : true
 *       },
 *       ...
 *     },
 *     ...
 *   }
 * }
 *
 * "disabled" describes whether the node / worker is healthy or not.
 * Even when requested, disabled will be omitted if node / worker is healthy to
 * minimize response size.
 * Response doesn't include workers at this point (TODO t7757044).
 */

dynamic HTTPMonitor::handleNodes(const Config& c, const dynamic& request) {
  dynamic responses = dynamic::object("results", dynamic::object());

  bool no_node_filter =
    request.getDefault("nodes", dynamic::array()).empty();
  std::unordered_set<folly::fbstring> do_this_node;
  if (!no_node_filter) {
    for (auto name : request["nodes"]) {
      do_this_node.insert(name.asString());
    }
  }

  bool field_disabled = true, field_resources = true;
  if (!request.getDefault("fields", dynamic::array()).empty()) {
    field_disabled = field_resources = false;
    for (const auto& field : request["fields"]) {
      const auto& fieldName = field.asString();
      if (fieldName == "resources") {
        field_resources = true;
      } else if (fieldName == "disabled") {
        field_disabled = true;
      } else {
        throw BistroException("Unknown field name: ", fieldName);
      }
    }
  }

  // Iterate over all nodes instead of accessing only those requested.
  auto& results = responses["results"];
  for (const auto& node : *(nodesLoader_->getDataOrThrow())) {
    const auto& node_name = node->name();
    if (no_node_filter || do_this_node.count(node_name) > 0) {
      // Add level to response if it isn't there yet.
      const int level = node->level();
      auto levelString = c.levels.lookup(level);
      auto& d_level = results.setDefault(levelString);
      auto& res = (d_level[node_name] = dynamic::object);

      if (field_resources) {
        auto& resources = (res["resources"] = dynamic::object);
        for (auto r_id : c.levelIDToResourceID[level]) {
          const auto& r_name = c.resourceNames.lookup(r_id);
          auto& resource = (resources[r_name] = dynamic::object
            ("default", c.defaultJobResources[r_id])
            ("limit", c.resourcesByLevel[level][r_id])
          );
          const auto weight = c.resourceIDToWeight[r_id];
          if (weight > 0) {
            resource["weight"] = weight;
          }
        }
      }

      if (field_disabled && !node->enabled()) {
        res["disabled"] = true;
      }
    }
  }
  return responses;
}

dynamic HTTPMonitor::handleSortedNodeNames(const Config& c) {
  // Returns a dict of level => list of node names
  dynamic ret = dynamic::object;
  auto nodes = nodesLoader_->getDataOrThrow();
  for (const auto& n : *nodes) {
    const auto& level = c.levels.lookup(n->level());
    ret.setDefault(level, dynamic::array()).push_back(n->name());
  }
  return ret;
}

namespace {

unordered_set<string> getJobNameSet(const dynamic& d) {
  unordered_set<string> job_names;
  if (auto* p = d.get_ptr("jobs")) {
    for (const auto& j : *p) {
      job_names.emplace(j.asString());
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
    const Config& /*c*/,
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

}}
