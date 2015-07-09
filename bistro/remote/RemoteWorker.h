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

#include <folly/Optional.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bistro/bistro/remote/RemoteWorkerState.h"

namespace facebook { namespace bistro {

class RemoteWorkerUpdate;
class TaskStatus;

// We use this special task id for 'healthcheck' tasks, that are sent
// regularly to test whether each remote worker is functioning well.
// Hack: Duplicated in SubprocessTaskQueue.
const std::string kHealthcheckTaskJob = "__BISTRO_HEALTH_CHECK__";
// This special node ID for healthcheck tasks signals that the worker is new.
const std::string kHealthcheckTaskNewWorkerNode = "__NEW_WORKER__";

/**
 * Implements the stateful protocol used by the scheduler to interact with a
 * single remote worker (see README.worker_protocol).
 *
 * WARNING: Not thread-safe, the caller must provide its own mutex.
 */
class RemoteWorker {
public:

  /**
   * Given a heartbeat from a previously unknown worker, make a
   * RemoteWorker.  You should call processHeartbeat after construction.
   */
  RemoteWorker(int64_t cur_time, const cpp2::BistroWorker& w_new)
    : worker_(w_new),
      state_(cur_time),
      // New worker: trigger a healthcheck in the next updateState
      timeLastHealthcheckSent_(INT64_MIN),
      timeOfLastUnsureIfRunningCheck_(INT64_MIN),
      repeatsOfUnsureIfRunningCheck_(0) {
  }

  bool isHealthy() const {
    return state_.state_ == RemoteWorkerState::State::HEALTHY;
  }

  RemoteWorkerState::State getState() const {
    return state_.state_;
  }

  const cpp2::BistroWorker& getBistroWorker() const {
    return worker_;
  }

  /**
   * Returns folly::none if the heartbeat should be rejected. Note that the
   * update should still be processed.
   */
  folly::Optional<cpp2::SchedulerHeartbeatResponse> processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new
  );

  /**
   * Updates the worker's current health state, and potentially tells the
   * scheduler to take actions on its behalf.
   *
   * The scheduler should call this as frequently as it can afford, in order
   * for the various worker timeouts to trigger responsively.  This is also
   * called on every worker heartbeat.
   */
  void updateState(RemoteWorkerUpdate* update);

  /**
   * Used when the scheduler starts a task. Should be run atomically with
   * TaskStatuses::updateStatus.
   */
  void recordRunningTaskStatus(
    const cpp2::RunningTask& rt,
    const TaskStatus& status
  ) noexcept;

  /**
   * Used when the scheduler itself (in the absence of a status update from
   * the remote worker) decides that the task has failed.  Should be run
   * atomically with TaskStatuses::updateStatus.
   *
   * Note: loseRunningTasks does not use this function.
   *
   * This is separate from recordNonRunningTaskStatus because failures from
   * the scheduler might trip its checks and make it throw.  Specifically,
   * the worker owning this task may have been replaced by another worker,
   * which may still be in the NEW state (throws an exception), and that
   * worker would have a different ID (throws an exception).
   */
  void recordFailedTask(
    const cpp2::RunningTask& rt,
    const TaskStatus& status
  ) noexcept;

  /**
   * Processes remote ThriftMonitor::updateStatus requests, doing everything
   * that TaskStatuses::updateStatus does not do.  Should be run atomically
   * with TaskStatuses::updateStatus.
   *
   * Throws to have the remote worker retry this updateStatus later.
   * Returns false to avoid recording the update in TaskStatuses.
   *
   * In more detail, the contract is:
   *  - Throws if the scheduler or worker ID does not match.
   *  - Throws if an update comes from a NEW worker.
   *  - Logs & returns false on updates for running tasks with wrong
   *    invocation IDs.
   *  - Records health-check replies, and returns false.
   *  - Updates the RemoteWorker's internal "running" and "unsure if running"
   *    task lists.
   *
   * Reminder: if you modify this, you should log the updates for which you
   * return false, since TaskStatuses::updateStatus will not log them.
   */
  bool recordNonRunningTaskStatus(
    const cpp2::RunningTask& rt,
    const TaskStatus& status,
    const cpp2::BistroInstanceID& worker_id
  );

  /**
   * When a new worker connects, it may already have running tasks. Register
   * those.  You must then update TaskStatuses while the worker is locked.
   */
  void initializeRunningTasks(const std::vector<cpp2::RunningTask>&);

  void addUnsureIfRunningTask(const cpp2::RunningTask& rt);
  void eraseUnsureIfRunningTasks(const std::vector<cpp2::RunningTask>& tasks);

  int64_t calledNotifyIfTasksNotRunning() {
    return ++state_.notifyIfTasksNotRunningSequenceNum_;
  }
  int64_t getNotifyIfTasksNotRunningSequenceNum() const {
    return state_.notifyIfTasksNotRunningSequenceNum_;
  }

private:
  typedef std::pair<std::string, std::string> TaskID;

  RemoteWorker() = default;

  /**
   * This call is const so that processHealthcheck can check the correct
   * would-be state of the current worker without altering it.
   */
  RemoteWorkerState::State computeState(int64_t cur_time) const {
    return state_.computeState(
      cur_time,
      state_.maxHealthcheckGap(),
      worker_.heartbeatPeriodSec + state_.heartbeatGracePeriod(),
      state_.loseUnhealthyWorkerAfter()
    );
  }

  /**
   * Got a heartbeat from a new worker instance. If there was a previous
   * worker, we already made sure it is dead (as are its tasks, see
   * README.task_termination).
   */
  void updateNewWorker(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new
  );

  /**
   * Got a heartbeat from the current worker instance.
   */
  void updateCurrentWorker(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new
  );

  /**
   * When our worker is marked "must die" or is replaced by a new one, it is
   * told to suicide, and its tasks are lost.  This clears runningTasks_ and
   * notifies TaskStatuses via the update.
   */
  void loseRunningTasks(RemoteWorkerUpdate* update);

  // Helps implement recordNonRunningTaskStatus and recordFailedTask
  bool recordNonRunningTaskStatusImpl(
    const cpp2::RunningTask& rt,
    const TaskStatus& status
  ) noexcept;

  cpp2::BistroWorker worker_;
  RemoteWorkerState state_;

  // TODO(lo-pri): Add exponential backoff to health checks. Otherwise, as
  // we churn workers, we will accumulate a lot of dead shard IDs that we'll
  // health-check pointlessly, wasting resources.  For now, restarts fix it.
  int64_t timeLastHealthcheckSent_;  // For --healthcheck_period

  // Running tasks per worker, redundant with TaskStatuses. These enable us
  // to efficiently lose tasks from lost workers (at present, it seems that
  // no other per-worker lookup of tasks is used).  DO: Keeping this
  // consistent with TaskStatuses requires a fair bit of attention (see
  // loseRunningTasks & RemoteWorkerRunner), so it may be preferable to lose
  // tasks from lost workers by brute force, and to delete this member.  I
  // only realized this after finishing this feature.  It also seems good
  // for UI queries, preemption (for coscheduling or task migration), etc.
  std::unordered_map<TaskID, cpp2::RunningTask> runningTasks_;

  // runTask() failed when scheduling these, so while we treat them as
  // running, we don't actually know if they are alive or dead, and will
  // periodically poll the worker for their status (until the check succeeds
  // or the worker is lost).
  std::unordered_map<TaskID, cpp2::RunningTask> unsureIfRunningTasks_;
  int64_t timeOfLastUnsureIfRunningCheck_;
  uint8_t repeatsOfUnsureIfRunningCheck_;  // for exponential backoff
};

}}
