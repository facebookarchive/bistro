/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <boost/noncopyable.hpp>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Config;
class Job;
class Monitor;
class Node;
class Nodes;
class TaskStatusSnapshot;

typedef std::shared_ptr<const Job> JobPtr;

class Scheduler : boost::noncopyable {
public:
  struct Result {
    bool areTasksRunning_;  // Lets us conserve CPU when idle.
    std::vector<cpp2::RunningTask> orphanTasks_;  // May be killed.
  };

  Result schedule(
    time_t cur_time,
    const Config& config,
    const std::shared_ptr<const Nodes>& nodes,
    const TaskStatusSnapshot& status_snapshot,
    TaskRunnerCallback cb,
    const std::shared_ptr<Monitor> monitor
  );
};

}}  // namespace facebook::bistro
