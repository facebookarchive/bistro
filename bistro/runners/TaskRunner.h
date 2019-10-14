/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/scheduler/ResourceVector.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include <folly/Conv.h>
#include <folly/json.h>

namespace facebook { namespace bistro {

class Config;
class Job;
class Node;
class TaskStatus;

namespace cpp2 {
  class RunningTask;
  class BistroInstanceID;
}

/**
 * TaskRunner::runTask() should return one of these.
 */
enum TaskRunnerResponse {
  // We ran the task and want to run more. Record that we're using this
  // task's resources.
  RanTask,
  // We couldn't run this task, but want to try running more
  DidNotRunTask,
  // We couldn't run this task, and will keep failing for now
  DoNotRunMoreTasks,
};

using TaskRunnerCallback = std::function<
  TaskRunnerResponse(const std::shared_ptr<const Job>&, const Node&)
>;

/**
 * Performs the action we take to actually 'run' the task.
 *
 * updateConfig() is called next, before we schedule a batch of jobs. This
 * is where the TaskRunner can update any state that depends on the config
 * (such as settings).
 *
 * As soon as a task is scheduled, we call runTaskImpl(), which should use
 * the callback to set a TaskStatus on that task.  If it successfully
 * started the task, it must call cb(rt, TaskStatus::running()), or we will
 * try to re-schedule the task again.  On error, it must call cb(rt, ...)
 * with the appropriate status, and it must never throw.
 *
 * You must set the task to 'running' **before** you make any external
 * service calls (possibly in a separate thread) to start the task. (see
 * the note on atomicity in README.remote_runners).
 *
 * When your task finishes, whether locally or externally, invoke the
 * callback with an error, done, or incomplete status based on the result
 * of its execution.
 *
 * Your TaskRunner must take care to only transition tasks from 'running' to
 * 'not running', and from 'not running' to 'running'.  We only enforce this
 * via logspam, because in the distributed world of README.remote_runners,
 * it is possible for the scheduler's state to be forcibly corrected by
 * remote workers.  In those cases, a clean running/not running toggle is
 * not guaranteed.
 */
class TaskRunner {

public:
  TaskRunner();
  virtual ~TaskRunner() {}

  // Used by child classes to do work before each scheduling loop.
  virtual void updateConfig(std::shared_ptr<const Config> /*config*/) {}

  TaskRunnerResponse runTask(
    const Config& config,
    const std::shared_ptr<const Job>& job,
    const Node& node,
    const TaskStatus* prev_status,
    // Must be thread-safe
    std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
  ) noexcept;

  virtual LogLines getJobLogs(
      const std::string& /*logtype*/,
      const std::vector<std::string>& /*jobs*/,
      const std::vector<std::string>& /*nodes*/,
      int64_t /*line_id*/,
      bool /*is_ascending*/,
      const std::string& /*regex_filter*/
      ) const {
    return LogLines();
  }

  virtual bool canKill() { return false; }

  /**
   * Kill a task -- blocks until the first signal is sent, does not wait for
   * the kill to succeed.  Throws if the signal cannot be sent.
   */
  virtual void killTask(const cpp2::RunningTask&, const cpp2::KillRequest&) {
    throw std::runtime_error("Killing tasks is not supported");
  }

  // The next two are used to implement TaskRunners with remote workers.
  // They are documented in if/scheduler.thrift.  See README.remote_runners
  // and README.worker_protocol for the semantics.

  /**
   * Your processWorkerHeartbeat() should always set the .id field to
   * schedulerID_.  You can throw to reject the heartbeat.
   */
  virtual cpp2::SchedulerHeartbeatResponse processWorkerHeartbeat(
    const cpp2::BistroWorker&,
    const cpp2::WorkerSetID&,
    RemoteWorkerUpdate /* update */ = RemoteWorkerUpdate(),
    std::function<void()> /* unit_test_cob */ = []() {}
  ) { throw std::logic_error("Not implemented"); }

  /**
   * Called by ThritfMonitor::updateStatus for remote workers to update both
   * TasksStatuses, and the internal RemoteWorker data structures.  If this
   * throws, the remote worker will re-send the status update.
   */
  virtual void remoteUpdateStatus(
      const cpp2::RunningTask& /*rt*/,
      TaskStatus&& /*status*/,
      const cpp2::BistroInstanceID /*scheduler_id*/,
      const cpp2::BistroInstanceID /*worker_id*/
      ) {
    throw std::logic_error("remoteUpdateStatus not implemented");
  }

 protected:

  // Used by RemoteWorkerRunner to record worker resources
  // Returns a pointer to the just-added RunningTask::nodeResources entry,
  // or nullptr if no resources exist for this node.
  static cpp2::NodeResources* addNodeResourcesToRunningTask(
    cpp2::RunningTask* out_rt,
    folly::dynamic* out_resources_by_node,
    const Config& config,
    const std::string& node_name,
    const int node_level,
    const ResourceVector& job_resources
  ) noexcept;

  /**
   * Implement this: the actual work of starting a task.
   *
   * You must mark the task as 'running' as soon as possible, but certainly
   * before dispatching^ it to any kind of asynchronous execution, via:
   *
   *   cb(rt, TaskStatus::running());
   *
   * Then, your goal job is to return a response code ASAP, so the scheduler
   * can continue dispatching tasks.  You must also invoke the callback with
   * the appropriate termination status.  Since tasks take a long time, this
   * is usually done asynchronously, much later, via a closure.
   *
   * If appropriate, you should also set rt.workerShard before marking the
   * task running.
   *
   * ^ The note on atomicity in README.remote_runners explains why it's
   *   important to mark as running ASAP.
   */
  virtual TaskRunnerResponse runTaskImpl(
    const std::shared_ptr<const Job>& job,
    const Node& node,
    cpp2::RunningTask& rt,
    folly::dynamic& job_args,
    // Must be thread-safe
    std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
  ) noexcept = 0;

  // Identifies this Bistro instance to remote workers, if any.
  const cpp2::BistroInstanceID schedulerID_;
};

}}
