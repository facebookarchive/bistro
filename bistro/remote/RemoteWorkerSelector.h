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

#include <unordered_map>

#include "bistro/bistro/runners/TaskRunner.h"  // TaskRunnerResponse
#include "bistro/bistro/config/RemoteWorkerSelectorType.h"
#include "bistro/bistro/scheduler/ResourceVector.h"

namespace facebook { namespace bistro {

namespace cpp2 {
  class BistroWorker;
}

class Job;
class Node;
class Monitor;
class RemoteWorkers;

class RemoteWorkerSelector {
public:
  typedef std::unordered_map<std::string, ResourceVector> WorkerResources;

  static RemoteWorkerSelector* getSingleton(RemoteWorkerSelectorType type);

  virtual ~RemoteWorkerSelector();

  TaskRunnerResponse findWorker(
    const Job& job,
    const Node& node,
    int worker_level,
    Monitor* monitor,
    WorkerResources* worker_resources,
    RemoteWorkers* workers,
    cpp2::BistroWorker* found_worker,
    int64_t* did_not_run_sequence_num
  ) noexcept;

//  virtual XXX = 0;

protected:
  // Tries to run the given job on the given worker, subject to available
  // worker resource.  If it's possible, returns true and updates the
  // passed-in worker resources.
  bool tryToRun(
    const Job& job,
    const cpp2::BistroWorker& worker,
    int worker_level,
    WorkerResources* worker_resources
  ) noexcept;
};

}}  // namespace facebook::bistro
