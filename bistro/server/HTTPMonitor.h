/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <string>
#include <thread>

#include "bistro/bistro/server/HTTPServer.h"
#include <folly/dynamic.h>

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
  void wait();

private:
  std::string handleRequest(const std::string& request);
  folly::dynamic handleSingle(const Config&, const folly::dynamic& d);
  folly::dynamic handleJobs(
    const Config& c,
    const std::vector<const Job*>& jobs
  );
  folly::dynamic handleNodes(const Config& c);
  folly::dynamic handleTaskRuntime(const folly::dynamic& d);
  folly::dynamic handleRunningTasks(const folly::dynamic& d);
  folly::dynamic handleHistograms(
    const Config& c,
    const std::vector<const Job*>& jobs,
    int num_samples
  );
  folly::dynamic handleTaskLogs(const Config&, const folly::dynamic& d);

  std::shared_ptr<ConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> taskStatuses_;
  std::shared_ptr<TaskRunner> taskRunner_;
  std::shared_ptr<Monitor> monitor_;

  HTTPServer server_;
  std::thread serverThread_;

};

}}
