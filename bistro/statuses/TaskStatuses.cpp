/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/statuses/TaskStatuses.h"

#include <folly/GLog.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatusObserver.h"

DEFINE_int32(
  log_status_changes_every_ms, 0,
  "0 to log all status changes; otherwise log only if this number "
  "of milliseconds passed since the last log."
);

namespace facebook { namespace bistro {

using apache::thrift::debugString;
using namespace std;
using namespace folly;

TaskStatuses::TaskStatuses(std::shared_ptr<TaskStore> task_store)
  : taskStore_(task_store),
    snapshot_(TaskStatusSnapshot(task_store)) {
}

void TaskStatuses::updateStatus(
    const Job::ID job_id,
    const Node::ID node_id,
    const cpp2::RunningTask& rt,
    TaskStatus&& status) noexcept {

  // TODO(#4813858): Improve FB_LOG_EVERY_MS, get rid of this conditional?
  if (FLAGS_log_status_changes_every_ms) {
    FB_LOG_EVERY_MS(INFO, FLAGS_log_status_changes_every_ms)
      << "Got status " << status.toJson() << " for " << debugString(rt)
      << " (sampled every " << FLAGS_log_status_changes_every_ms << "ms)";
  } else {
    LOG(INFO) << "Got status " << status.toJson() << " for "
      << debugString(rt);
  }

  // Copies the stored status, since snapshot_ is modified concurrently.
  auto stored_status =
    snapshot_->updateStatus(job_id, node_id, rt, std::move(status));

  // Work with new_status rather than status because we might set the new
  // status to "failed" if it runs out of backoff attempts.
  recordStatusUpdate(rt, stored_status);
}

// Helper for updateStatus, to prevent accidental use of the moved status.
void TaskStatuses::recordStatusUpdate(
    const cpp2::RunningTask& rt,
    const TaskStatus& new_status) noexcept {

  for (auto observer : statusObservers_) {
    // TaskStatusObservers shouldn't interfere with each other.
    try {
      observer->updateTaskStatus(rt, new_status);
    } catch (const exception& e) {
      LOG(ERROR) << observer->name() << " task status update failed: "
        << e.what();
    }
  }
  if (taskStore_) {
    try {
      if (new_status.isDone()) {
        taskStore_->store(rt.job, rt.node, TaskStore::TaskResult::DONE);
      } else if (new_status.isFailed()) {
        taskStore_->store(rt.job, rt.node, TaskStore::TaskResult::FAILED);
      }
    } catch (const exception& e) {
      // TODO: Add exponential backoff retry? Crashing doesn't actually
      // protect us from incorrect behavior here -- the moment we fail to
      // persist something, we can potentially re-run done / failed tasks, or
      // start duplicate tasks on scheduler restart.  Having good retry logic
      // to cover transient failures would improve reliability noticeably.
      CHECK(false) << "Failed to persist status: " << e.what();
    }
  }
}

void TaskStatuses::updateStatus(
    const cpp2::RunningTask& rt,
    TaskStatus&& status) noexcept {

  // Insert the IDs, because they might not exist yet.
  const int job_id = Job::JobNameTable->insert(rt.job);
  const int node_id = Node::NodeNameTable->insert(rt.node);
  updateStatus(Job::ID(job_id), Node::ID(node_id), rt, std::move(status));
}

void TaskStatuses::updateForConfig(const Config& config) {
  snapshot_->updateForConfig(config);
}

// TODO(#5555238): We should also erase the "failed" row from TaskStore.
void TaskStatuses::forgiveJob(const string& job) {
  const int job_id = as_const(Job::JobNameTable)->lookup(job);
  if (job_id == StringTable::NotFound) {
    throw BistroException("Invalid job id: ", job);
  }
  snapshot_->forgiveJob(Job::ID(job_id));
}

}}
