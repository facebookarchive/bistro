/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/remote/RemoteWorker.h"

#include <folly/GLog.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/remote/WorkerSetID.h"
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

bool checkWorkerSchedulerProtocolVersion(int16_t worker, int16_t scheduler) {
  // In the future, this check can allow some limited backward/forward
  // compatibility for specific versions (as needed).
  return worker == scheduler;
}

void enforceWorkerSchedulerProtocolVersion(int16_t worker, int16_t scheduler) {
  if (!checkWorkerSchedulerProtocolVersion(worker, scheduler)) {
    throw std::runtime_error(folly::to<std::string>(
      "Worker-scheduler protocol version mismatch: ", worker,
      " is not compatible with ", scheduler
    ));
  }
}

folly::Optional<cpp2::SchedulerHeartbeatResponse>
RemoteWorker::processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new,
    const cpp2::WorkerSetID& worker_set_id,
    bool consensus_permits_becoming_healthy) {

  // We should never get here (since that means the worker is already added
  // to our pool), so CHECK instead of throwing.
  CHECK(checkWorkerSchedulerProtocolVersion(
    w_new.protocolVersion, cpp2::common_constants::kProtocolVersion()
  )) << "Worker & scheduler protocol mismatch: " << w_new.protocolVersion
    << " vs " << cpp2::common_constants::kProtocolVersion();
  const auto& w_cur = worker_;
  // We'll have to kill the old worker, unless the new process is running on
  // the same machine and listening on the same port as the old.
  if (w_cur.machineLock != w_new.machineLock) {
    // Bump the current worker if it's unhealthy or "must die".
    auto state_and_disallowed =
      computeState(update->curTime(), consensus_permits_becoming_healthy);
    if (state_and_disallowed.first == RemoteWorkerState::State::MUST_DIE) {
      // The worker may not yet have been marked MUST_DIE by updateState, so
      // this suicide request is necessary.  At worst, it's a no-op.
      update->requestSuicide(w_cur, "Current lost worker was replaced");
      // Fall through to updateNewWorker()
    } else if (
      FLAGS_allow_bump_unhealthy_worker
      // Also bumps RemoteWorkerState::State::NEW workers, in case, e.g. a
      // NEW worker is stuck in failing to report its running tasks.
      && state_and_disallowed.first != RemoteWorkerState::State::HEALTHY
    ) {
      if (state_and_disallowed.second) {
        LOG(INFO) << "Current worker " << w_cur.shard << " is unhealthy "
          << "solely because it lacks WorkerSetID consensus. It will be "
          << "replaced by a new worker";
      }
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
    // Ignore consensus_permits_becoming_healthy since it's not relevant.
    updateNewWorker(update, w_new, worker_set_id);
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
    // Ignore consensus_permits_becoming_healthy since it's not relevant.
    updateNewWorker(update, w_new, worker_set_id);
  // It's the same worker instance as before, just update the metadata.
  } else {
    // Got a heartbeat from a MUST_DIE worker -- re-request suicide
    if (state_.state_ == RemoteWorkerState::State::MUST_DIE) {
      update->requestSuicide(w_cur, "Current worker was already lost");
    }
    updateCurrentWorker(
      update, w_new, worker_set_id, consensus_permits_becoming_healthy
    );
  }
  return state_.getHeartbeatResponse();
}

void RemoteWorker::updateState(
    RemoteWorkerUpdate* update,
    bool consensus_permits_becoming_healthy) {
  // MUST_DIE means we don't check health, and ignore state changes.
  if (state_.state_ == RemoteWorkerState::State::MUST_DIE) {
    return;  // Don't request suicide here since we updateState very often
  }
  auto new_state_and_disallowed =
    computeState(update->curTime(), consensus_permits_becoming_healthy);
  auto new_state = new_state_and_disallowed.first;
  if (new_state_and_disallowed.second) {
    LOG(INFO) << "Worker " << worker_.shard << " can be healthy but lacks "
      << "WorkerSetID consensus";
  }
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
    deadWorkerCob_(*this);
    loseRunningTasks(update);
    setState(RemoteWorkerState::State::MUST_DIE);
    return;
  }
  if (state_.state_ != RemoteWorkerState::State::HEALTHY
      && new_state == RemoteWorkerState::State::HEALTHY) {
    LOG(INFO) << "Worker " << worker_.shard << " became healthy";
  }
  setState(new_state);
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
    const cpp2::BistroWorker& w_new,
    const cpp2::WorkerSetID& worker_set_id) {

  if (state_.state_ != RemoteWorkerState::State::MUST_DIE) {
    deadWorkerCob_(*this);
  }
  // Any previous worker would have been killed, so make sure to lose those
  // running tasks (in the pathological case where the new worker has some
  // of the same running tasks, this logs "lost" followed by "running").
  loseRunningTasks(update);
  *this = RemoteWorker(
    update->curTime(),
    w_new,
    worker_set_id,
    std::move(schedulerID_),
    std::move(newWorkerCob_),
    std::move(deadWorkerCob_),
    std::move(workerSetIDChangeCob_)
  );
  // Read the 'This should never happen' comment in updateCurrentWorker.
  if (worker_set_id.schedulerID == schedulerID_) {
    LOG(ERROR) << "The scheduler ID of the initial WorkerSetID of "
      << debugString(worker_) << " is the same as the current scheduler: "
      << debugString(worker_set_id);
  }
  state_.timeLastHeartbeatReceived_ = update->curTime();
  updateState(update, /*the consensus computation wasn't yet done: */ false);
}

void RemoteWorker::updateCurrentWorker(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& w_new,
    const cpp2::WorkerSetID& worker_set_id,
    bool consensus_permits_becoming_healthy) {

  worker_ = w_new;
  // The scheduler might get a stale heartbeat containing a WorkerSetID from
  // before this worker adopted this scheduler's worker set versioning.
  // This would wreak havoc, so avoid it.
  //
  // NB: This isn't called from BistroWorkerHandler, so no worries about that.
  if (worker_set_id.schedulerID == schedulerID_) {
    // This should never happen, but I cannot quite rule it out. Roughly
    // speaking, this means that the first time this worker instance
    // connected (as far as the scheduler is concerned), it was already
    // associated with this scheduler.  In this case, logging and doing
    // nothing matches the behavior of the updateNewWorker() code path.  In
    // particular, that means we preserve the invariant that workerSetID_ is
    // always a later version than initialWorkerSetID_, and always contains
    // the present worker.
    if (worker_set_id.schedulerID == initialWorkerSetID_.schedulerID
        && !WorkerSetIDEarlierThan()(
          initialWorkerSetID_.version, worker_set_id.version
        )) {
      LOG(ERROR) << "The scheduler ID of the initial WorkerSetID of "
        << debugString(worker_) << " is the same as the current scheduler: "
        << debugString(initialWorkerSetID_) << " -- ignoring the current "
        << "WorkerSetID since it is not later than the initial: "
        << debugString(worker_set_id);
    // Only increase the version, since the worker only increases its
    // internal versions.  A decrease means out-of-order arrival.
    //
    // NB: This criterion could be used to discard all such heartbeats, but
    // the extra complexity isn't worth the dubious gain.
    } else if (
        !workerSetID_.has_value() ||
        WorkerSetIDEarlierThan()(
            workerSetID_->version, worker_set_id.version)) {
      workerSetIDChangeCob_(*this, worker_set_id);
      if (!firstAssociatedWorkerSetID_.has_value()) {
        CHECK(!workerSetID_.has_value());
        CHECK(initialWorkerSetID_ != worker_set_id)
          << debugString(initialWorkerSetID_) << " == "
          << debugString(worker_set_id);
        firstAssociatedWorkerSetID_ = worker_set_id;
      }
      workerSetID_ = worker_set_id;
    } else if (workerSetID_->version == worker_set_id.version) {
      CHECK(*workerSetID_ == worker_set_id)
        << debugString(*workerSetID_) << " != " << debugString(worker_set_id);
    } else { // This can happen occasionally (e.g. under heavy load).
      LOG(WARNING) << "Ignoring out-of-order WorkerSetID -- current: "
        << debugString(*workerSetID_) << ", new: "
        << debugString(worker_set_id);
    }
  // This equality will hold just after the RemoteWorker was first created,
  // but any other kind of mismatch should not happen, so log those.
  } else if (worker_set_id != initialWorkerSetID_) {
    LOG(ERROR) << "Scheduler " << debugString(schedulerID_) << " got "
      << "a heartbeat with a WorkerSetID whose schedulerID does not match: "
      << debugString(w_new) << " / " << debugString(worker_set_id);
  }
  state_.timeLastHeartbeatReceived_ = update->curTime();
  updateState(update, consensus_permits_becoming_healthy);
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
  // thread that calls updateState.  DO: It could be nicer to immediately
  // trigger updateState in the caller, but then one also has to apply the
  // resulting update, which increases the code complexity a lot.
  setState(RemoteWorkerState::State::UNHEALTHY);
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
  // either MUST_DIE or NEW (if the old worker dies and is replaced by a new
  // invocation).  The only way to leave these states is through
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
