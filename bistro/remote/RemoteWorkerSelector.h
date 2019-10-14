/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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

  virtual TaskRunnerResponse findWorker(
    const Config* config,
    const Job& job,
    const Node& node,
    int worker_level,
    Monitor* monitor,
    WorkerResources* worker_resources,
    RemoteWorkers* workers,
    cpp2::BistroWorker* found_worker,
    // getNotifyIfTasksNotRunningSequenceNum() from the chosen worker.
    int64_t* did_not_run_sequence_num
  ) noexcept = 0;

  // Check whether the job's filters prevent it from running on this worker.
  static bool jobCanRunOnWorker(
    const Job& job,
    const cpp2::BistroWorker& worker,
    int worker_level
  ) noexcept;
};

}}  // namespace facebook::bistro
