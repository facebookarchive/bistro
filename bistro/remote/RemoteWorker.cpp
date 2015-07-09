/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/remote/RemoteWorker.h"

#include <folly/Logging.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/Exception.h"

DEFINE_bool(
  allow_bump_unhealthy_worker, false,
  "If we get a heartbeat from a new worker, and the current worker is "
  "unhealthy, but not lost, should we lose the current worker immediately? "
  "The default (false) means we follow the normal heartbeat conflict "
  "resolution protocol, which favors keeping the current worker. The upside "
  "of setting this to true is that worker failovers will happen faster. "
  "The downside is that we're slightly less reliable in the event that the "
  "unhealthy worker does not get the suicide request, but does come back up."
);

DEFINE_int32(
  log_healthchecks_every_ms, 0,
  "0 to log all successful healthchecks; otherwise log only if this number "
  "of milliseconds passed since the last log. Always log failed healthchecks."
);

DEFINE_int32(
  unsure_if_running_check_initial_period, 1,
  "If the scheduler hits an error running a task, and does not know whether "
  "the task was started, it will poll the worker with this initial period "
  "and a 2x exponential backoff every retry up to a maximum of 256x."
);

namespace facebook { namespace bistro {

namespace {
  const uint8_t kMaxBackoffForUnsureIfRunningCheck = 8;  // 256 * initial wait
}

using apache::thrift::debugString;
using namespace std;

folly::Optional<cpp2::SchedulerHeartbeatResponse>
RemoteWorker::processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new) {

  const auto& w_cur = worker_;
  // We'll have to kill the old worker, unless the new process is running on
  // the same machine and listening on the same port as the old.
  if (w_cur.machineLock != w_new.machineLock) {
    // Bump the current worker if it's unhealthy or "must die".
    auto cur_state = computeState(update->curTime());
    if (cur_state == RemoteWorkerState::State::MUST_DIE) {
      // The worker may not yet have been marked MUST_DIE by updateState, so
      // this suicide request is necessary.  At worst, it's a no-op.
      update->requestSuicide(w_cur, "Current lost worker was replaced");
      // Fall through to updateNewWorker()
    } else if (
      FLAGS_allow_bump_unhealthy_worker
      // Also bumps RemoteWorkerState::State::NEW workers, in case, e.g. a
      // NEW worker is stuck in failing to report its running tasks.
      && cur_state != RemoteWorkerState::State::HEALTHY
    ) {
      update->requestSuicide(w_cur, "Current unhealthy worker was replaced");
      // Fall through to updateNewWorker()
    } else {
      // The currently associated worker is okay, so kill the other worker
      // that's trying to talk to us.  The suicide request is important to
      // try to ensure that no tasks are running on workers that are not
      // managed by the scheduler.  For example, we may have failed over to
      // a new worker due to a network partition, but now the partition got
      // fixed, and the previous worker came back.  This suicide request
      // should reap it.
      //
      // TODO(#5023846): Unfortunately, it is possible that the other worker
      // has running tasks, but is _not_ yet associated with this scheduler
      // (e.g. the scheduler restarted since the network partition
      // happened).  In that case, the suicide request will fail, and the
      // worker will continue to run tasks, and to try to associate itself
      // with the scheduler (up until it becomes lost and kills itself).
      // This time-to-fail is too long.  Probably, the worker should just
      // auto-associate itself to the new scheduler when its requests get
      // routed there.
      update->requestSuicide(w_new, "The old worker is still okay");
      return folly::none;
    }
    updateNewWorker(update, w_new);
  // A new worker instance at the same IP & port. Easy to handle, since
  // we're pretty sure that the current one is dead -- no need to check
  // health, or tell anyone to commit suicide.
  } else if (w_cur.id != w_new.id) {
    // Ignore heartbeats coming from the dead worker (due to network lag?)
    if (w_cur.id.startTime > w_new.id.startTime) {
      LOG(WARNING) << "Ignored dead worker heartbeat: " << debugString(w_new);
      return folly::none;
    }
    // The worker has a deliberate 1-s startup delay, so start times
    // should never be equal for distinct worker instances on one machine.
    if (w_cur.id.startTime == w_new.id.startTime) {
      LOG(ERROR) << "Heartbeat error: same start time "
        << w_cur.id.startTime << " but different rands: " << w_cur.id.rand
        << ", " << w_new.id.rand;
    }
    updateNewWorker(update, w_new);
  // It's the same worker instance as before, just update the metadata.
  } else {
    // Got a heartbeat from a MUST_DIE worker -- re-request suicide
    if (state_.state_ == RemoteWorkerState::State::MUST_DIE) {
      update->requestSuicide(w_cur, "Current worker was already lost");
    }
    updateCurrentWorker(update, w_new);
  }
  return state_.getHeartbeatResponse();
}

void RemoteWorker::updateState(RemoteWorkerUpdate* update) {
  // MUST_DIE means we don't check health, and ignore state changes.
  if (state_.state_ == RemoteWorkerState::State::MUST_DIE) {
    return;  // Don't request suicide here since we updateState very often
  }
  auto new_state = computeState(update->curTime());
  // Careful: RemoteWorkerRunner::checkInitialWait relies on the fact that
  // this check populates addNewWorker **every** updateState.
  if (new_state == RemoteWorkerState::State::NEW) {
    // Fetch running tasks from this worker, and send a special first
    // healthcheck (see BistroWorkerHandler::runTask).
    update->addNewWorker(worker_);
    // DO: This is printed every time we retry fetching running tasks, which
    // looks a bit confusing in the logs.
    LOG(INFO) << "Initializing new worker: " << debugString(worker_);
    // Go on below to see if it's time for a a healthcheck.
  }
  if (new_state != RemoteWorkerState::State::HEALTHY
      && state_.state_ == RemoteWorkerState::State::HEALTHY) {
    LOG(WARNING) << "Worker " << worker_.shard << " became unhealthy";
    state_.timeBecameUnhealthy_ = update->curTime();
  }
  if (new_state == RemoteWorkerState::State::MUST_DIE) {
    // Send a suicide request the moment we declare the worker lost (and
    // also on any heartbeat we receive from it thereafter).
    update->requestSuicide(worker_, "Current worker just became lost");
    loseRunningTasks(update);
    state_.state_ = RemoteWorkerState::State::MUST_DIE;
    return;
  }
  if (state_.state_ != RemoteWorkerState::State::HEALTHY
      && new_state == RemoteWorkerState::State::HEALTHY) {
    LOG(INFO) << "Worker " << worker_.shard << " became healthy";
  }
  state_.state_ = new_state;
  // Send out a new healtcheck if we are due.
  if (update->curTime() >= timeLastHealthcheckSent_
      + max(1, FLAGS_healthcheck_period)) {
    update->healthcheckWorker(worker_);
    timeLastHealthcheckSent_ = update->curTime();
  }
  // Check 'unsure if running' tasks, if they are due.
  if (unsureIfRunningTasks_.empty()) {
    repeatsOfUnsureIfRunningCheck_ = 0;  // reset exponential backoff
  } else if (update->curTime() >= timeOfLastUnsureIfRunningCheck_ + (
    std::max(1, FLAGS_unsure_if_running_check_initial_period) <<
      repeatsOfUnsureIfRunningCheck_
  )) {
    update->checkUnsureIfRunningTasks(worker_, unsureIfRunningTasks_);
    timeOfLastUnsureIfRunningCheck_ = update->curTime();
    if (repeatsOfUnsureIfRunningCheck_ < kMaxBackoffForUnsureIfRunningCheck) {
      ++repeatsOfUnsureIfRunningCheck_;
    }
  }
}

void RemoteWorker::recordRunningTaskStatus(
    const cpp2::RunningTask& rt,
    const TaskStatus& status) noexcept {

  CHECK(status.isRunning());

  auto task_id = TaskID(rt.job, rt.node);
  // An "unsure if running" task should never be marked "running"
  CHECK(unsureIfRunningTasks_.count(task_id) == 0) << debugString(rt);
  // Unlike the analogous check in TaskStatusSnapshot::updateStatus, a task
  // should never, ever be started on a worker that is already running it.
  CHECK(runningTasks_.emplace(task_id, rt).second) << debugString(rt);
  // DO: CHECK that the state is HEALTHY?
}

void RemoteWorker::recordFailedTask(
    const cpp2::RunningTask& rt,
    const TaskStatus& status) noexcept {

   CHECK(!status.isRunning() && !status.isDone());
  // DO: Consider CHECKing that the task is currently in 'runningTasks_'?
  // DO: Maybe also CHECK that the task is not in unsureIfRunningTasks_?
  CHECK(recordNonRunningTaskStatusImpl(rt, status));
}

// IMPORTANT: Read the contract in RemoteWorker.h befor modifying this.
bool RemoteWorker::recordNonRunningTaskStatus(
    const cpp2::RunningTask& rt,
    const TaskStatus& status,
    const cpp2::BistroInstanceID& worker_id) {

  CHECK(!status.isRunning());

  // Log and reject statuses from non-current workers.
  if (worker_id != worker_.id) {
    throw BistroException(
      "Got status for ", rt.job, ", ", rt.node, " from an unexpected worker: ",
      worker_id.startTime, ", ", worker_id.rand
    );
  }

  // Process healthcheck responses
  if (rt.job == kHealthcheckTaskJob) {
    // Update the "last good" time only for successful healthchecks
    if (status.isDone()) {
      state_.timeLastGoodHealthcheckSent_ =
        max(rt.invocationID.startTime, state_.timeLastGoodHealthcheckSent_);
      // TODO(#4813858): Improve FB_LOG_EVERY_MS, get rid of this conditional?
      if (FLAGS_log_healthchecks_every_ms) {
        FB_LOG_EVERY_MS(INFO, FLAGS_log_healthchecks_every_ms)
          << "Good healthcheck from " << rt.workerShard << " sent "
          << rt.invocationID.startTime << " (sampled every "
          << FLAGS_log_healthchecks_every_ms << "ms)";
      } else {
        LOG(INFO) << "Good healthcheck from " << rt.workerShard << " sent "
          << rt.invocationID.startTime;
      }
    } else {
      LOG(ERROR)
        << "Unsuccessful healthcheck status " << status.toJson() << " from "
        << rt.workerShard << " sent " << rt.invocationID.startTime;
    }
    // Neither this class nor TaskStatuses keeps track of outstanding
    // healthchecks.
    return false;
  }

  // Reject NEW workers, since we don't want their updateStatus results to
  // be clobbered by the imminent getRunningTasks results.
  //
  // N.B. We might also request suicide for MUST_DIE workers, but it's too
  // much work in the current setup.  Also, keep in mind that the remote
  // updateStatus from this MUST_DIE (but undead) worker will be currently
  // recorded, improving upon a plain "lost" status.
  if (state_.state_ == RemoteWorkerState::State::NEW) {
    throw BistroException(
      "Rejecting updateStatus from a worker in NEW state, must wait until "
      "it retries: ", debugString(rt)
    );
  }

  return recordNonRunningTaskStatusImpl(rt, status);
}

// Must not return false in situations where recordFailedTask is used, i.e.
// whenever the scheduler decides on its own that a task has failed.
bool RemoteWorker::recordNonRunningTaskStatusImpl(
    const cpp2::RunningTask& rt,
    const TaskStatus& status) noexcept {

  auto task_id = TaskID(rt.job, rt.node);
  auto it = runningTasks_.find(task_id);
  if (it != runningTasks_.end()) {
    // It is possible that this updateStatus message does not refer to the
    // currently running task invocation.  The only way that can happen is if
    // the message was massively delayed on the network, and a new task
    // invocation has started since then.
    //
    // The correct response is to quietly drop the message, preventing
    // re-sends by the worker, and also not to record the update via
    // TaskStatuses (and hence TaskStatusObservers).
    if (it->second.invocationID != rt.invocationID) {
      LOG(WARNING) << "Ignoring severely delayed updateStatus message with "
        << "incorrect invocation ID, new task " << debugString(rt)
        << " vs new task " << debugString(it->second) << " with status "
        << status.toJson();
      return false;
    }
    runningTasks_.erase(it);
  } else if (status.isOverwriteable()) {
    // The task is already not running, so don't overwrite its possibly
    // application-generated status with a less meaningful synthetic one.
    // Log here, since TaskStatuses::updateStatus will not be called.  It is
    // appropriate to test for overwriteable statuses in remote-worker code,
    // since they are exclusive to remote workers.
    //
    // NB: This code path is not used by loseRunningTasks, so this status
    // for sure originates with a notifyIfTasksNotRunning call.
    LOG(INFO) << "Ignoring overwriteable status for " << debugString(rt)
      << ": " << status.toJson();
    return false;
  } else {
    // Rarely, we will end up here due to the worker retrying updateStatus
    // after a "partial failure" -- the scheduler recording the status, but
    // the worker not getting the acknowledgement.
    LOG(ERROR) << "RemoteWorker already knew that " << debugString(rt)
      << " was NOT running";
  }

  // We're now sure the task is not running.
  unsureIfRunningTasks_.erase(task_id);

  // At this point, either we are updating a running task, and its
  // invocation ID matches, or we're updating a task that already became
  // "not running" via a prior updateStatus, see e.g. [delay] and [replay]
  // in its docblock.  We'll let TaskStatuses::updateStatus deal with this.
  return true;
}

// NB: Explicitly call requestSuicide in processHeartbeat, and not here
// since it would be spammy in the "same host, same port, new worker" case.
void RemoteWorker::updateNewWorker(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new) {

  // Any previous worker would have been killed, so make sure to lose those
  // running tasks (in the pathological case where the new worker has some
  // of the same running tasks, this logs "lost" followed by "running").
  loseRunningTasks(update);
  *this = RemoteWorker(update->curTime(), w_new);
  updateState(update);
}

void RemoteWorker::updateCurrentWorker(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new) {

  worker_ = w_new;
  state_.timeLastHeartbeatReceived_ = update->curTime();
  updateState(update);
}

void RemoteWorker::initializeRunningTasks(
    const std::vector<cpp2::RunningTask>& running_tasks) {

  CHECK(state_.state_ == RemoteWorkerState::State::NEW);

  CHECK(runningTasks_.empty())
    << "Had " << runningTasks_.size() << " running tasks, starting with "
    << debugString(runningTasks_.begin()->second) << ", but expected 0.";
  CHECK(unsureIfRunningTasks_.empty())
    << "Had " << unsureIfRunningTasks_.size() << " rather than 0 'unsure if "
    << "running' tasks, starting with "
    << debugString(unsureIfRunningTasks_.begin()->second);

  runningTasks_.reserve(running_tasks.size());
  for (const auto& rt : running_tasks) {
    CHECK(rt.workerShard == worker_.shard)
      << "Bad shard for new task " << worker_.shard << ": " << debugString(rt);
    CHECK(runningTasks_.emplace(TaskID(rt.job, rt.node), rt).second)
      << "Duplicate new task for worker " << debugString(rt);
  }

  // Exit the "NEW" state; the true state will be computed by the periodic
  // thread that calls updateState.  DO: It would be nicer to call
  // updateState immediately in the casller, but then one also has to apply
  // the resulting update, which increases the code complexity a lot.
  state_.state_ = RemoteWorkerState::State::UNHEALTHY;
}

void RemoteWorker::loseRunningTasks(RemoteWorkerUpdate* update) {
  for (const auto& pair : runningTasks_) {
    update->loseRunningTask(pair.first, pair.second);
  }
  // We'll reset runningTasks_ and unsureIfRunningTasks_ atomically with the
  // RemoteWorker state transition.  Unfortunately, the corresponding
  // TaskStatuses updates have to happen later, so the system will go through
  // these 3 states:
  //
  //   a) RemoteWorker not lost & TaskStatuses not updated
  //   b) RemoteWorker is lost, but TaskStatuses not updated
  //   c) RemoteWorker is lost & TaskStatuses updated
  //
  // Since the two states are out of sync in (b), that's the only place
  // where problems can arise.  When a worker is lost, its state becomes
  // either MUST_DIE or NEW.  The only way to leave these states is through
  // fetchRunningTasksForNewWorkers, which runs after lost running tasks
  // processing in applyUpdate, which means that when we change states, we
  // are surely in (c).  This is the synchronization mechanism that ensures
  // that (a) -> (c) is effectively atomic.
  //
  // To avoid taking this on faith, let's systematically go through the
  // callsites that access runningTasks_, unsureIfRunningTasks_ or
  // TaskStatuses:
  //   - RemoteWorkerRunner::remoteUpdateStatus: Throws, forcing a retry, if
  //     the RemoteWorker is in NEW. Allows through remote non-running
  //     updates for MUST_DIE workers, because those updates are more
  //     meaningful than the default "lost" status we are about to generate,
  //     and still consistent with the RemoteWorker's state of "the task is
  //     not running".
  //   - RemoteWorkerRunner::runTaskImpl: Only uses HEALTHY workers.
  //   - RemoteWorkerRunner::checkUnsureIfRunningTasks: In applyUpdate, this
  //     precedes fetchRunningTasksForNewWorkers, but we explicitly clear
  //     update->unsureIfRunningTasks_ below, and no new ones will be
  //     generated by runTaskImpl until we get to (c).
  //   - RemoteWorkerRunner::fetchRunningTasksForNewWorkers: Runs after
  //     applyUpdate processes lost workers, putting us in (c).
  //
  // TODO(#5507329): Maybe RemoteWorker could act directly on TaskStatuses?
  runningTasks_.clear();
  unsureIfRunningTasks_.clear();
  // DO: Consider checking these invariants for this worker in "update":
  //  - suicide was requested
  //  - no healthcheck was requested
  //  - there are no "unsure if running" tasks to check
  //  - we are not trying to fetch new tasks for the worker
}

void RemoteWorker::addUnsureIfRunningTask(const cpp2::RunningTask& rt) {
  CHECK(unsureIfRunningTasks_.emplace(TaskID(rt.job, rt.node), rt).second)
    << "Was already unsure about running status of task " << debugString(rt);
}

// We cannot just call .clear(), because new ones may have been added while
// notifyIfTasksNotRunning() was being called.
void RemoteWorker::eraseUnsureIfRunningTasks(
  const std::vector<cpp2::RunningTask>& tasks) {

  for (const auto& rt : tasks) {
    CHECK(rt.workerShard == worker_.shard)
      << "Bad shard for 'unsure if running' task " << worker_.shard << ": "
      << debugString(rt);
    auto it = unsureIfRunningTasks_.find(TaskID(rt.job, rt.node));
    // The warnings should happen rarely; they could be due to a race
    // between updateStatus and notifyIfTasksNotRunning.
    if (it == unsureIfRunningTasks_.end()) {
      LOG(WARNING) << "Polled worker for an 'unsure if running' task, but "
        << "afterwards it was not in the list: " << debugString(rt);
    } else if (it->second.invocationID != rt.invocationID) {
      LOG(WARNING) << "Polled worker for one 'unsure if running' task, but "
        << "when the reply came, its invocation ID did not match, old "
        << debugString(rt) << " vs " << debugString(it->second);
    } else {
      unsureIfRunningTasks_.erase(it);
    }
  }
}

}}
