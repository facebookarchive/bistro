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

#include <boost/noncopyable.hpp>
#include <folly/Synchronized.h>
#include <memory>
#include <thread>
#include <thrift/lib/cpp/async/TEventBase.h>

#include "bistro/bistro/remote/RemoteWorkers.h"
#include "bistro/bistro/runners/TaskRunner.h"
#include "bistro/bistro/scheduler/ResourceVector.h"
#include "bistro/bistro/utils/BackgroundThreadMixin.h"

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro {

namespace cpp2 {
  class BistroWorker;
  class BistroWorkerAsyncClient;
}

class Config;
class Job;
class LogLines;
class Monitor;
class Node;
class TaskStatus;
class TaskStatuses;

class RemoteWorkerRunner
  : boost::noncopyable, public TaskRunner, BackgroundThreadMixin {

public:
  explicit RemoteWorkerRunner(
    std::shared_ptr<TaskStatuses> task_statuses,
    std::shared_ptr<Monitor> monitor
  );
  ~RemoteWorkerRunner() override;

  /**
   * Recomputes the available worker resources, in case they were reconfigured
   */
  void updateConfig(std::shared_ptr<const Config> config) override;

  /**
   * Aggregates log lines matching the request by querying all workers.
   *
   * Returns some number of log lines starting from the given line_id.
   * To page through all lines, take this to be your next line_id:
   *   log.nextLineID (kNotALine if there are no more lines)
   */
  LogLines getJobLogs(
    const std::string& logtype,
    const std::vector<std::string>& jobs,
    const std::vector<std::string>& nodes,
    int64_t line_id,  // min if is_ascending, max otherwise
    bool is_ascending,
    const std::string& regex_filter
  ) override;

  bool canKill() override { return true; }

  void killTask(
    const std::string& job,
    const std::string& node,
    cpp2::KilledTaskStatusFilter status_filter
  ) override;

  cpp2::SchedulerHeartbeatResponse processWorkerHeartbeat(
    const cpp2::BistroWorker&,
    RemoteWorkerUpdate update = RemoteWorkerUpdate()  // for unit test
  ) override;

  void remoteUpdateStatus(
    const cpp2::RunningTask& rt,
    TaskStatus&& status,
    const cpp2::BistroInstanceID scheduler_id,
    const cpp2::BistroInstanceID worker_id
  ) override;

  // used in unit test only
  cpp2::BistroInstanceID getSchedulerID() const {
    return schedulerID_;
  }

// TODO: Make this private once we don't have an FB-specific class
// inheriting from this.
protected:
 TaskRunnerResponse runTaskImpl(
     const std::shared_ptr<const Job>& job,
     const std::shared_ptr<const Node>& node,
     cpp2::RunningTask& rt,
     folly::dynamic& job_args,
     std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)>
         cb) noexcept override;

private:
  /**
   * At startup, the scheduler has to wait for workers to connect, and to
   * report their running tasks, so that we do not accidentally re-start
   * tasks that are already running elsewhere.
   *
   * Should only be used from the background thread, **after** update is
   * populated by RemoteWorkers::updateState.  Other places should use
   * inInitialWait_.load(std::memory_order_relaxed).
   */
  void checkInitialWait(const RemoteWorkerUpdate& update);

  // Thrift helpers
  std::shared_ptr<cpp2::BistroWorkerAsyncClient> getWorkerClient(
    const cpp2::BistroWorker& w
  );
  void sendWorkerHealthcheck(const cpp2::BistroWorker&, bool) noexcept;
  void requestWorkerSuicide(const cpp2::BistroWorker& w) noexcept;

  /**
   * RemoteWorkers collect their side effects in RemoteWorkerUpdate. Here,
   * we apply those side effects to Bistro's state.  For each new worker,
   * adds running tasks to the update.
   */
  void applyUpdate(RemoteWorkerUpdate* update);
  // Helper for applyUpdate
  void checkUnsureIfRunningTasks(
    const cpp2::BistroWorker& w,
    const std::vector<cpp2::RunningTask>& tasks
  );
  // Helper for applyUpdate
  void fetchRunningTasksForNewWorkers(RemoteWorkerUpdate* update);

  // Tracks worker identity, health, and current running tasks.
  // CAUTION: For dual locks, lock workerResources_ first.
  folly::Synchronized<RemoteWorkers> workers_;

  // Remaining resources for each worker shard, based on the currently
  // running tasks.  This map is computed using the memoized config_.
  // CAUTION: For dual locks, lock workers_ second.
  folly::Synchronized<std::unordered_map<std::string, ResourceVector>>
    workerResources_;
  // Memoized: the copy we got from the last updateConfig. For consistency,
  // we should never use any other Config together with workerResources_.
  std::shared_ptr<const Config> config_;
  // Micro-optimization: which config level has the worker resources & filters?
  // Memoized so that we don't pull it from Config for each task we run.
  int workerLevel_;

  // The RemoteWorkerRunner is the ground truth for running tasks, so it
  // directly updates TaskStatuses based on RemoteWorkerUpdates.
  std::shared_ptr<TaskStatuses> taskStatuses_;

  // This base belongs to the thread below -- don't use it elsewhere.  It's
  // used to queue and process "fire and forget" async communications with
  // the workers, i.e.  runTask, healthcheck, requestSuicide.  In contrast,
  // getJobLogs runs synchronously in the request handler's thread.
  std::unique_ptr<apache::thrift::async::TEventBase> eventBase_;
  std::thread eventBaseThread_;

  // When the scheduler restarts, don't start running tasks right away,
  // because previous workers may have previously running tasks.  Since this
  // only experiences one write, true => false, it's fine to use
  // std::memory_order_relaxed with all accesses.
  std::atomic<bool> inInitialWait_;
  // Used to enforce the initial wait.
  time_t startTime_;

  // Used to report errors to the UI, can be null.
  std::shared_ptr<Monitor> monitor_;
};

}}
