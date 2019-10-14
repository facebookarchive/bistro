/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Optional.h>
#include <folly/Synchronized.h>
#include <memory>
#include <string>
#include <thread>

#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStatusSnapshot.h"

namespace facebook { namespace bistro {

class Config;
class Job;
class Node;
class JobBackoffSettings;
typedef std::shared_ptr<const Job> JobPtr;
class TaskStore;
class TaskStatusObserver;
namespace cpp2 {
  class RunningTask;
}

/**
 * Stores and updates statuses for running and completed tasks.
 *
 * This is a synchronized wrapper around the one true TaskStatusSnapshot,
 * Bistro's ground truth for all statuses it keeps in memory.  To avoid
 * contention, be frugal with how you access it, and especially with
 * copySnapshot().
 */
class TaskStatuses {
public:
  explicit TaskStatuses(std::shared_ptr<TaskStore> task_store);
  ~TaskStatuses() {}

  void addObserver(std::shared_ptr<TaskStatusObserver> observer) {
    statusObservers_.push_back(observer);
  }

  /**
   * Modifies in-memory snapshot of statuses, and/or list of running tasks.
   */
  void updateStatus(
    const Job::ID job_id,
    const Node::ID node_id,
    const cpp2::RunningTask& rt,
    TaskStatus&& status
  ) noexcept;

  /**
   * Looks up / creates the job & node IDs if they don't exist already.
   *
   * The noexcept is a genuine attempt never to throw from this code path,
   * though things like std::bad_alloc can obviously still happen, aborting
   * the program -- but we won't try to salvage those situations.
   */
  void updateStatus(const cpp2::RunningTask& rt, TaskStatus&& status) noexcept;

  TaskStore& getTaskStore() {
    return *taskStore_;
  }

  /**
   * The cheapest way to look up one running task.
   */
  folly::Optional<cpp2::RunningTask> copyRunningTask(
    Job::ID job_id,
    Node::ID node_id
  ) const {
    folly::Optional<cpp2::RunningTask> maybe_rt;
    SYNCHRONIZED(snapshot_) {
      auto it = snapshot_.runningTasks_.find(std::make_pair(job_id, node_id));
      if (it != snapshot_.runningTasks_.end()) {
        maybe_rt = it->second;
      }
    }
    return maybe_rt;
  }

  /**
   * The cheapest way to get many running tasks. Way cheaper than copySnapshot.
   */
  const std::unordered_map<std::pair<Job::ID, Node::ID>, cpp2::RunningTask>
  copyRunningTasks() const {
    std::unordered_map<std::pair<Job::ID, Node::ID>, cpp2::RunningTask> rts;
    SYNCHRONIZED(snapshot_) {
      rts = snapshot_.runningTasks_;
    }
    return rts;
  }

  /**
   * Get all statuses & running tasks. Very expensive, call sparingly.
   */
  TaskStatusSnapshot copySnapshot() const {
    return snapshot_.copy();
  }

  /**
   * Loads persisted statuses for new jobs, and forgets statuses for jobs
   * that are no longer configured.  Does not affect {get,copy}RunningTasks,
   * but does impact the general-purpose accessors getPtr() and getRow().
   * Don't use those to check if a task is running!
   */
  void updateForConfig(const Config& config);

  /**
   * Make all of the job's tasks eligible to run immediately.  If the
   * current status uses backoff, resets the duration to 0.  Replaces
   * permanent failure with "error, but can run again".
   */
  void forgiveJob(const std::string& job);

private:
  // Helper for updateStatus, to prevent accidental use of the moved status.
  void recordStatusUpdate(
    const cpp2::RunningTask& rt,
    const TaskStatus& new_status
  ) noexcept;

  std::shared_ptr<TaskStore> taskStore_;
  std::vector<std::shared_ptr<TaskStatusObserver>> statusObservers_;

  folly::Synchronized<TaskStatusSnapshot> snapshot_;
};

}}
