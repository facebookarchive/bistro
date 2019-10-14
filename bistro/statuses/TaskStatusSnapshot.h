/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

class Config;
class TaskStore;
namespace detail {
  class TaskStatusRow;
}

/**
 * An unsynchronized data structure that stores all the known task statuses
 * at a specific point in time.  There is a unique TaskStatuses object,
 * which emits a copy of its "ground truth" TaskStatusSnapshot any time that
 * a thread needs to do some nontrivial computation on statuses (e.g.
 * scheduling, monitor histogram, etc.).
 *
 * So, this class has only the core data accessors, while TaskStatuses
 * expresses the application logic relating to statuses.
 *
 * TODO: It's dubious that getPtr() simply returns nullptr for out-of-range
 * job or node IDs; this should probably be a hard-crash.
 */
class TaskStatusSnapshot {
  struct StatusRow {
    StatusRow() : isLoaded_(false) {}
    // Until we load this job's failed/done statuses from the DB, and check
    // runningTasks_ for previously running tasks, getPtr() will return null
    // for this job.
    bool isLoaded_;
    // Each job stores its statuses as a vector "mapping" node id to status
    std::vector<TaskStatus> statuses_;
  };

public:

  // Only public for the tests -- use TaskStatuses and copySnapshot elsewhere
  explicit TaskStatusSnapshot(std::shared_ptr<TaskStore> task_store)
    : taskStore_(task_store) {}

  /**
   * IMPORTANT: Calling either getPtr() variant for jobs that are known but
   * not loaded will result in immediate program termination.
   *
   * Returns false for jobs with too-high IDs, even though getPtr will
   * currently succeed for those (see above TODO).
   */
  bool isJobLoaded(const Job::ID job_id) {
    int jid = static_cast<int>(job_id);
    return jid < rows_.size() && rows_[jid].isLoaded_;
  }

  const TaskStatus* getPtr(Job::ID job_id, Node::ID node_id) const;

  detail::TaskStatusRow getRow(Job::ID job_id) const;

  const std::unordered_map<std::pair<Job::ID, Node::ID>, cpp2::RunningTask>&
  getRunningTasks() const {
    return runningTasks_;
  }

private:
  friend class TaskStatuses;
  friend class detail::TaskStatusRow;

  /**
   * Toggles a task between "running" and "not running" states. Logs when
   * the running/not running alternation is violated (this almost certainly
   * indicates a bug in your TaskRunner).
   *
   * Returns a copy of the resulting stored status.
   */
  TaskStatus updateStatus(
    const Job::ID job_id,
    const Node::ID node_id,
    const cpp2::RunningTask& rt,
    TaskStatus&& status
  ) noexcept;

  /**
   * When jobs get added or deleted, we need to either load heir statuses
   * from the TaskStore, or to clear the snapshot row to save memory.
   * runningTasks_ is not modified, since we need to keep them for proper
   * resource accounting.
   */
  void updateForConfig(const Config& config);

  void forgiveJob(const Job::ID job_id);

  inline TaskStatus& access(int job_id, int node_id);

  // If a job is deleted and re-added, we will need to reload its statuses.
  std::shared_ptr<TaskStore> taskStore_;

  // All statuses are stored as a vector "mapping" job id to a status row
  std::vector<StatusRow> rows_;

  // There are four places in Bistro that track whether a task is running:
  //
  //  a) In the case of remote workers, the worker holding a running task
  //     is the absolute authority on that RunningTask. Bistro's state
  //     is periodically synchronized with the worker's via heartbeats.
  //
  //  b) The runTask() callback must feed a RunningTask into updateStatus.
  //     That's Bistro's ground truth, but it cannot be queried.
  //
  //  c) runningTasks_ here is a queriable cache of Bistro's ground truth,
  //     The **set** of running job-nodes is supposed to match the running
  //     tasks we will receive via the runTask callback, but the runners are
  //     allowed to change the RunningTask metadata before the callback, to
  //     the extent it makes sense (e.g. nextBackoffDuration is fair game).
  //     With remote workers, this map is periodically updated via
  //     updateStatus.  While a task is running, Bistro uses this cache of
  //     RunningTasks for resource accounting and monitoring.
  //     TaskStatus::updateStatus gets the true RunningTask from (b).
  //
  //  d) TaskStatusSnapshot::rows_, which is used by schedule() to
  //     efficiently skip over already-running tasks.  It stores a 'data'
  //     field for TaskStatusObservers.
  //
  // TODO: For most uses of RunningTasks, e.g. HTTPMonitor, updateForConfig,
  // and RemoteWorkerRunner, it would be cleaner to have a two-step lookup
  // (by job, then by node).  However, this might slow down the scheduling
  // loop.  Profile and refactor if that's false.
  std::unordered_map<std::pair<Job::ID, Node::ID>, cpp2::RunningTask>
    runningTasks_;
};

namespace detail {

class TaskStatusRow {

public:
  explicit TaskStatusRow(const TaskStatusSnapshot::StatusRow* r) : row_(r) {}

  const TaskStatus* getPtr(Node::ID node_id) const;

private:
  const TaskStatusSnapshot::StatusRow* row_;

};

}

}}
