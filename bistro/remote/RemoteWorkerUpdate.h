/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

/**
 * Collects actions requested by RemoteWorker instances so that
 * RemoteWorkerRunner can execute them.  Designed so that multiple workers
 * can safely accumulate updates here.
 *
 * Mutators return false if any of the mutations are logical duplicates
 * (indicative of a bug or transient error), and also log.  Duplicates are
 * then ignored.
 *
 * DO: Replace the duplicate detection code with CHECKs, and we're not using
 * the boolean return values anyhow.
 */
class RemoteWorkerUpdate {
public:
  enum UnitTestTime { UNIT_TEST_TIME };

  typedef std::unordered_map<std::string, cpp2::BistroWorker> WorkerMap;
  typedef std::pair<std::string, std::string> TaskID;
  typedef std::unordered_map<TaskID, cpp2::RunningTask> TaskMap;
  typedef std::unordered_map<
    std::string, std::pair<cpp2::BistroWorker, std::vector<cpp2::RunningTask>>
  > UnsureIfRunningTaskMap;

  RemoteWorkerUpdate()
    : curTime_(time(nullptr)), initialWaitMessage_("unknown") {}
  RemoteWorkerUpdate(UnitTestTime, int64_t t)
    : curTime_(t), initialWaitMessage_("unknown") {}

  int64_t curTime() const { return curTime_; }

  const WorkerMap& workersToHealthcheck() const {
    return workersToHealthcheck_;
  }
  const WorkerMap& suicideWorkers() const { return suicideWorkers_; }
  const WorkerMap& newWorkers() const { return newWorkers_; }

  const TaskMap& lostRunningTasks() const { return lostRunningTasks_; }

  const UnsureIfRunningTaskMap& unsureIfRunningTasksToCheck() const {
    return unsureIfRunningTasks_;
  }

  // Worker actions

  bool healthcheckWorker(const cpp2::BistroWorker& w) {
    if (!workersToHealthcheck_.emplace(w.shard, w).second) {
      LOG(ERROR) << "Worker healthcheck requested more than once: " << w.shard;
      return false;
    }
    return true;
  }

  bool requestSuicide(const cpp2::BistroWorker& w, const std::string& reason) {
    LOG(INFO) << "Telling " << apache::thrift::debugString(w)
      << " to commit suicide: " << reason;
    if (!suicideWorkers_.emplace(w.shard, w).second) {
      LOG(ERROR) << "Worker suicide requested more than once: " << w.shard;
      return false;
    }
    return true;
  }

  bool addNewWorker(const cpp2::BistroWorker& w) {
    if (!newWorkers_.emplace(w.shard, w).second) {
      LOG(ERROR) << "New worker added more than once: " << w.shard;
      return false;
    }
    return true;
  }

  // Task actions

  bool loseRunningTask(const TaskID& task_id, const cpp2::RunningTask& rt) {
    if (!lostRunningTasks_.emplace(task_id, rt).second) {
      LOG(ERROR) << "Task was lost more than once: "
        << rt.job << ", " << rt.node << " on " << rt.workerShard;
      return false;
    }
    return true;
  }

  void checkUnsureIfRunningTasks(
      const cpp2::BistroWorker& worker,
      const TaskMap& task_map) {

    std::vector<cpp2::RunningTask> tasks;
    tasks.reserve(task_map.size());
    for (const auto& p : task_map) {
      tasks.push_back(p.second);
    }
    if (!unsureIfRunningTasks_.emplace(
      worker.shard, std::make_pair(worker, std::move(tasks))
    ).second) {
      LOG(ERROR) << "Unsure-if-running tasks check requested more than once "
        << "for " << worker.shard;
    }
  }

  const std::string& initialWaitMessage() const { return initialWaitMessage_; }
  void setInitialWaitMessage(std::string m) {
    initialWaitMessage_ = std::move(m);
  }

private:
  const int64_t curTime_;

  // Worker actions

  // A RemoteWorker can request to be health-checked by adding its info here.
  WorkerMap workersToHealthcheck_;
  // Which workers should we ask to kill themselves?
  WorkerMap suicideWorkers_;
  // We fetch running tasks from new workers. They also get a special
  // healthcheck (see BistroWorkerHandler::runTask).
  WorkerMap newWorkers_;

  // Task actions

  // IDs of tasks that we thought were running, but the worker did not.
  TaskMap lostRunningTasks_;

  // RemoteWorker wants us to ask the remote worker if these are running.
  UnsureIfRunningTaskMap unsureIfRunningTasks_;

  // Empty once the scheduler exits initial wait.
  std::string initialWaitMessage_;
};

}}
