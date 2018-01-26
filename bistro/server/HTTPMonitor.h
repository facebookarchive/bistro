/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/dynamic.h>
#include <string>
#include <thread>

namespace facebook { namespace bistro {

class TaskRunner;
class ConfigLoader;
class NodesLoader;
class Scheduler;
class TaskStatuses;
class Monitor;
class Config;
class Job;

class HTTPMonitor {

public:
  HTTPMonitor(
    std::shared_ptr<ConfigLoader> config_loader,
    std::shared_ptr<NodesLoader> nodes_loader,
    std::shared_ptr<TaskStatuses> task_statuses,
    std::shared_ptr<TaskRunner> task_runner,
    std::shared_ptr<Monitor> monitor
  );

  HTTPMonitor(const HTTPMonitor&) = delete;
  HTTPMonitor(HTTPMonitor&&) = delete;
  HTTPMonitor& operator=(const HTTPMonitor&) = delete;
  HTTPMonitor& operator=(HTTPMonitor&&) = delete;

  // Public only for unit tests
  folly::dynamic handleNodes(const Config& c, const folly::dynamic& request);

  folly::fbstring handleRequest(const folly::fbstring& request);

private:
  folly::dynamic handleSingle(const Config&, const folly::dynamic& d);
  folly::dynamic handleJobs(
    const Config& c,
    const std::vector<const Job*>& jobs
  );

  folly::dynamic handleSortedNodeNames(const Config& c);
  folly::dynamic handleTaskRuntime(const folly::dynamic& d);
  folly::dynamic handleRunningTasks(const folly::dynamic& d);
  folly::dynamic handleHistograms(
    const Config& c,
    const std::vector<const Job*>& jobs,
    int num_samples
  );
  folly::dynamic handleTaskLogs(const Config&, const folly::dynamic& d);

  // We assume all of these are thread safe themselves, so we don't do any
  // locking in the HTTPMonitor itself.
  std::shared_ptr<ConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> taskStatuses_;
  std::shared_ptr<TaskRunner> taskRunner_;
  std::shared_ptr<Monitor> monitor_;
};

}}
