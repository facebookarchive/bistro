/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/statuses/TaskStatusSnapshot.h"

#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/utils/Exception.h"
#include <folly/experimental/AutoTimer.h>

namespace facebook { namespace bistro {

using namespace std;
using apache::thrift::debugString;

// Careful: this does not check isLoaded_, but your calling function should?
inline TaskStatus& TaskStatusSnapshot::access(int j, int n) {
  const int job_id = static_cast<int>(j);
  const int node_id = static_cast<int>(n);
  if (job_id >= rows_.size()) {
    rows_.resize(job_id + 1);
  }
  auto& row = rows_[job_id];
  if (node_id >= row.statuses_.size()) {
    row.statuses_.resize(node_id + 1);
  }
  return row.statuses_[node_id];
}

void TaskStatusSnapshot::updateForConfig(const Config& config) {
  // Make a set of current jobs so we can test if jobs were deleted.
  unordered_set<int> current_jobs;
  for (const auto& pair : config.jobs) {
    current_jobs.insert(static_cast<int>(pair.second->id()));
  }

  // Look for running tasks that don't have corresponding status entries in
  // rows.  This happens, e.g. when a job gets removed and re-added.
  for (const auto& id_and_task : runningTasks_) {
    const auto& rt = id_and_task.second;
    const int job_id = Job::JobNameTable.asConst()->lookup(rt.job);
    const int node_id = Node::NodeNameTable.asConst()->lookup(rt.node);
    auto& status_ref = access(job_id, node_id);
    if (!status_ref.isRunning()) {
      LOG(WARNING) << "Task in runningTasks_ was not marked as running: "
        << status_ref.toJson();
    }
    status_ref = TaskStatus::running();
    // Even if the job is deleted from the configuration, don't clear out
    // its status just yet.
    current_jobs.insert(job_id);
  }

  // To save RAM, delete status rows for jobs that are not current, and have
  // no running tasks.  Very deliberately does not modify runningTasks_,
  // since we still have to track those tasks until they finish.
  folly::AutoTimer<> timer;
  int count = 0, last_deleted = StringTable::NotFound;
  for (int job_id = 0; job_id < rows_.size(); ++job_id) {
    if (current_jobs.count(job_id) == 0 && !rows_[job_id].statuses_.empty()) {
      rows_[job_id] = StatusRow();
      ++count;
      last_deleted = job_id;
    }
  }
  if (count > 0) {
    timer.log(
      "Cleared statuses for ", count, " deleted jobs, including ",
      Job::JobNameTable.asConst()->lookup(last_deleted)
    );
  }

  // Some current jobs might not have an entry in rows_ yet.
  int max_job_id = -1;
  for (auto job_id : current_jobs) {
    max_job_id = max(job_id, max_job_id);
  }
  if (max_job_id >= rows_.size()) {
    rows_.resize(max_job_id + 1);
  }

  // For current jobs that have not yet loaded statuses, read the statuses
  // from the TaskStore.  TODO: Overall plan for handling TaskStore failures.
  //
  // Load all the jobs in one batch because the per-call overhead of
  // TaskStore::fetchJobTasks can be high for remote DBs.
  std::vector<std::string> job_names;
  for (auto job_id : current_jobs) {
    if (!rows_[job_id].isLoaded_) {
      job_names.push_back(Job::JobNameTable->lookup(job_id));
    }
  }
  taskStore_->fetchJobTasks(
    job_names,
    [this](const string& job, const string& node, TaskStore::TaskResult r) {
      const int job_id = Job::JobNameTable.asConst()->lookup(job);
      CHECK(job_id != StringTable::NotFound) << "Job should be known: " << job;
      const int node_id = Node::NodeNameTable->insert(node);
      auto& status_ref = access(job_id, node_id);
      if (status_ref.isRunning()) {
        LOG(ERROR) << "Refusing to load status " << r << " for running task "
          << job << ", " << node;
      }
      if (r == TaskStore::TaskResult::DONE) {
        status_ref = TaskStatus::done();
      } else if (r == TaskStore::TaskResult::FAILED) {
        status_ref = TaskStatus::failed();
      } else {
        LOG(ERROR) << "Bad status " << r << " for " << job << ", " << node;
      }
    }
  );
  // Mark all jobs loaded
  for (auto job_id : current_jobs) {
    if (!rows_[job_id].isLoaded_) {
      rows_[job_id].isLoaded_ = true;
    }
  }
}

TaskStatus TaskStatusSnapshot::updateStatus(
    const Job::ID job_id,
    const Node::ID node_id,
    const cpp2::RunningTask& rt,
    TaskStatus&& status) noexcept {

  // We may update a row with isLoaded_ == false here, and that's ok.
  TaskStatus& stored_status =
    access(static_cast<int>(job_id), static_cast<int>(node_id));

  auto task_id = std::make_pair(job_id, node_id);
  auto it = runningTasks_.find(task_id);
  if (it != runningTasks_.end()) {
    // Cannot happen, RemoteWorker::recordNonRunningTaskStatus checks this.
    CHECK(it->second.invocationID == rt.invocationID)
      << "Cannot updateStatus since the invocation IDs don't match, new task "
      << debugString(rt) << " vs current task " << debugString(it->second)
      << " with status " << status.toJson();
  }

  if (status.isRunning()) {
    if (it != runningTasks_.end()) {
      // This used to happen "by design", because each heartbeat used to
      // include the list of running tasks.  Now, this should never happen,
      // since the transition to "running" is always initiated by the
      // scheduler, after checking the task is NOT in runningTasks_.  The
      // one exception is NEW workers; a non-NEW worker cannot become NEW,
      // because it would have been lost and told to suicide before being
      // de-associated with its worker shard ID.  Therefore, this could only
      // fire if the scheduler gets a NEW worker with a task that's
      // simultaneously being run on a current worker.
      //
      // DO: Test for & handle this case.
      //
      // However, in all normal situations, seeing this message means your
      // TaskRunner has a bug, and does not properly alternate running /
      // non-running statuses.
      LOG(ERROR) << "Task's status was already RUNNING: " << debugString(rt);
      // We already know that the task invocation IDs are the same, so there
      // is no point in re-inserting the task.
    } else {
      CHECK(runningTasks_.emplace(task_id, rt).second);  // should never fail
    }
  } else {  // The incoming status is not "running"
    // The previous status already wasn't "running"
    if (it == runningTasks_.end()) {
      // Cannot happen, since RemoteWorker::recordNonRunningTaskStatus is
      // supposed to filter out overwriteable statuses that would replace
      // an existing "not running" status.
      CHECK(!status.isOverwriteable());
      // A overwriteable status is safe to replace with the new one, it just
      // means that a normal updateStatus arrived after loseRunningTasks, or
      // after a notifyIfTasksNotRunning reply.
      if (stored_status.isOverwriteable()) {
        LOG(INFO) << "Replacing overwriteable " << stored_status.toJson()
          << " with a new status " << status.toJson();
      } else {
        // Rarely. we will end up here due to the worker retrying
        // updateStatus after a "partial failure" -- the scheduler recording
        // the status, but the worker not getting the acknowledgement.
        //
        // If seen frequently, this message may mean that your TaskRunner is
        // not properly alternating running / non-running statuses.
        //
        // DO: CHECK() that the stored and new status are the same?
        //
        // DO: It may be better if RemoteWorker::recordNonRunningTaskStatus
        // made this decision not to update this status, but currently it
        // has no access to the stored status.
        LOG(ERROR) << "Task " << debugString(rt)
          << " was already NOT running (" << stored_status.toJson()
          << ") but got an update of " << status.toJson();
        // Do *NOT* update the status, since that might unnecessarily
        // decrease the retry count.
        //
        // DO: Does it make sense to go and write this status to a
        // TaskStore, and pass it to TaskStatusObservers, as we do now?
        return stored_status;
      }
    } else {
      runningTasks_.erase(it);
    }
  }

  // Replace the old with the new; do this last since it moves the status.
  // May decrease the retry count.
  return stored_status.update(rt, std::move(status));
}

void TaskStatusSnapshot::forgiveJob(const Job::ID job_id) {
  int jid = static_cast<int>(job_id);
  if (jid < rows_.size()) {
    for (auto& status : rows_[jid].statuses_) {  // Need not check isLoaded_
      status.forgive();
    }
  }
}

const TaskStatus* TaskStatusSnapshot::getPtr(Job::ID jid, Node::ID nid) const {
  const int job_id = static_cast<int>(jid);
  const int node_id = static_cast<int>(nid);
  if (job_id >= rows_.size()) {
    return nullptr;
  }
  auto& row = rows_[job_id];
  CHECK(row.isLoaded_) << "Job not loaded "
    << Job::JobNameTable->lookup(job_id);
  if (node_id >= row.statuses_.size()) {
    return nullptr;
  }
  if (row.statuses_[node_id].isEmpty()) {
    return nullptr;
  }
  return &row.statuses_[node_id];
}

detail::TaskStatusRow TaskStatusSnapshot::getRow(Job::ID job_id) const {
  if (static_cast<int>(job_id) >= rows_.size()) {
    return detail::TaskStatusRow(nullptr);
  }
  auto* r = &rows_[static_cast<int>(job_id)];
  CHECK(r->isLoaded_) << "Job not loaded "
    << Job::JobNameTable->lookup(job_id);
  return detail::TaskStatusRow(r);
}

namespace detail {

const TaskStatus* TaskStatusRow::getPtr(Node::ID node_id) const {
  if (!row_ || static_cast<int>(node_id) >= row_->statuses_.size()) {
    return nullptr;
  }
  // getRow already checked isLoaded_
  const TaskStatus* s = &(row_->statuses_[static_cast<int>(node_id)]);
  if (s->isEmpty()) {
    return nullptr;
  }
  return s;
}

}

}}
