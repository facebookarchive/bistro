/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/worker/BistroWorkerHandler.h"

#include <boost/filesystem.hpp>
#include <folly/experimental/AutoTimer.h>
#include <folly/File.h>
#include <folly/json.h>
#include <folly/Random.h>
#include <limits>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"
#include "bistro/bistro/if/gen-cpp2/scheduler_types.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/utils/LogWriter.h"
#include "bistro/bistro/utils/service_clients.h"
#include "bistro/bistro/if/gen-cpp2/worker_types.h"

DEFINE_string(
  shard_id, "",
  "Only one worker with a given shard ID can be connected to the scheduler "
  "at any time. An empty value (the default) maps to the local hostname."
);
DEFINE_int32(
  heartbeat_period_sec, 15,
  "How often to send heartbeats to the scheduler. These include running "
  "tasks, so for large deployments, you may want to make this longer. The "
  "scheduler's startup wait MUST be at least a couple of heartbeat periods "
  "long to allow all pre-existing workers to connect before it starts "
  "to run tasks."
);
DEFINE_string(
  data_dir, "/data/bistro", "Where to create status pipes and job directories"
);
DEFINE_string(
  log_db_file_name, "task_logs.sql3",  // matches LocalRunner
  "One would rename the log DB when many workers use the same data_dir. "
  "Note that storing log DBs on a networked FS is guaranteed to reduce "
  "your overall system reliability."
);
DEFINE_bool(
  use_soft_kill, true,
  "To kill, send a task SIGTERM, wait, and only then SIGKILL"
);

namespace facebook { namespace bistro {

using apache::thrift::debugString;
using namespace folly;
using namespace std;

namespace {

template<typename... Args>
cpp2::BistroWorkerException BistroWorkerException(Args&&... args) {
  cpp2::BistroWorkerException ex;
  ex.message = folly::to<std::string>(std::forward<Args>(args)...);
  LOG(ERROR) << "Replying with exception: " << ex.message;
  return ex;
}

cpp2::BistroWorker makeWorker(
    const cpp2::ServiceAddress& addr,
    int32_t locked_port) {

  // This delay ensures that RemoteWorker::processHeartbeat will never see
  // two workers on the same host & port with the same start time.
  std::this_thread::sleep_for(std::chrono::milliseconds(1250));

  // This has to be the canonical FQDN for the local host.  Read the comment
  // for MachinePortLock to see why.
  auto hostname = getLocalHostName();
  cpp2::BistroWorker worker;
  worker.shard = FLAGS_shard_id.empty() ? hostname : FLAGS_shard_id;
  worker.machineLock.hostname = hostname;
  worker.machineLock.port = locked_port;
  worker.addr = addr;
  // Give the worker instance a unique ID.
  worker.id.startTime = time(nullptr);
  // Seeded from /dev/urandom, which is important given that it would be
  // pretty useless if rand correlated with startTime.
  worker.id.rand = folly::Random::rand64(folly::ThreadLocalPRNG());
  worker.heartbeatPeriodSec = FLAGS_heartbeat_period_sec;
  LOG(INFO) << "Worker is ready: " << debugString(worker);
  return worker;
}

}  // anonymous namespace

BistroWorkerHandler::BistroWorkerHandler(
    SchedulerClientFn scheduler_client_fn,
    const string& worker_command,
    const cpp2::ServiceAddress& addr,
    int32_t locked_port)
  : fb303::FacebookBase2("BistroWorker"),
    schedulerClientFn_(scheduler_client_fn),
    workerCommand_(worker_command),
    taskQueue_(
      boost::filesystem::path(FLAGS_data_dir) / ("/" + FLAGS_log_db_file_name),
      boost::filesystem::path(FLAGS_data_dir) / "/pipes"
    ),
    notifyFinishedQueue_(100000),
    notifyNotRunningQueue_(10000),
    jobsDir_(boost::filesystem::path(FLAGS_data_dir) / "/jobs"),
    worker_(makeWorker(addr, locked_port)),
    state_(RemoteWorkerState(worker_.id.startTime)),
    gotNewSchedulerInstance_(true),
    canConnectToMyself_(false) {

  // No scheduler associated yet, so use a dummy instance ID and timeouts
  schedulerState_->id.startTime = 0;
  schedulerState_->id.rand = 0;
  // Become unhealthy instantly
  schedulerState_->maxHealthcheckGap = 0;
  schedulerState_->heartbeatGracePeriod = 0;
  schedulerState_->workerCheckInterval = 0;
  // But take 60 years to get lost, so we don't suicide on startup.
  schedulerState_->loseUnhealthyWorkerAfter = numeric_limits<int32_t>::max();
  schedulerState_->workerState = static_cast<int>(
    RemoteWorkerState::State::NEW  // not used
  );

  runInBackgroundLoop(bind(&BistroWorkerHandler::healthcheck, this));
  runInBackgroundLoop(bind(&BistroWorkerHandler::heartbeat, this));
  runInBackgroundLoop(bind(&BistroWorkerHandler::notifyFinished, this));
  runInBackgroundLoop(bind(&BistroWorkerHandler::notifyNotRunning, this));
}

BistroWorkerHandler::~BistroWorkerHandler() {
  stopBackgroundThreads();
}

void BistroWorkerHandler::getRunningTasks(
    std::vector<cpp2::RunningTask>& out_running_tasks,
    const cpp2::BistroInstanceID& worker) {

  if (worker != worker_.id) {
    throw BistroWorkerException(
      "Worker ", worker_.id.startTime, "/" , worker_.id.rand,
      " ignored getRunningTasks for ", worker.startTime, "/", worker.rand
    );
  }

  SYNCHRONIZED_CONST(runningTasks_) {
    for (const auto& id_and_task : runningTasks_) {
      // The scheduler doesn't track healthcheck jobs, don't confuse it.
      if (id_and_task.second.job != kHealthcheckTaskJob) {
        out_running_tasks.push_back(id_and_task.second);
      }
    }
  }
  // state_->state_ will be changed from NEW by the scheduler's response to
  // our next heartbeat.  Arguably, we could also preemptively set it to
  // UNHEALTHY here.
}

// CONTRACT: Must only throw BistroWorkerException if the task will never
// start.  Must only return successfully if the task is enqueued and will
// certainly be reported to the scheduler via updateStatus.
void BistroWorkerHandler::runTask(
    const cpp2::RunningTask& rt,
    const string& config,
    const vector<string>& cmd,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    int64_t notify_if_tasks_not_running_sequence_num) {

  AutoTimer<> timer("runTask was slow");
  timer.setMinTimeToLog(0.1); // 100 ms per log => 10 tasks/sec

  bool isHealthcheck = rt.job == kHealthcheckTaskJob;
  // Tells the scheduler that we aren't even going to try running this.  Run
  // healthchecks even if we're unhealthy, because otherwise we'd never be
  // able to recover from missing a healthcheck.
  if (state_->state_ != RemoteWorkerState::State::HEALTHY && !isHealthcheck) {
    throw BistroWorkerException(
      "Unhealthy, not running task: ", rt.job, ", ", rt.node
    );
  }
  // Accept "new worker" healthchecks even if the scheduler ID doesn't
  // match.  This is important since those healthchecks often arrive just
  // before the initial heartbeat response (so we still have the old
  // scheduler ID).  Accepting them prevents unnecessary worker loss, and
  // speeds up failovers.
  //
  // If multiple schedulers concurrently try to talk to this worker (e.g.
  // due to operational errors), we don't want to blindly accept
  // healthchecks from both, as 50+% of the checks would fail.  Since we
  // only accept "new worker" healthchecks here, this risk is very low.
  //
  // TODO: An alternative is for every heartbeat to generate a fresh
  // 'alternative' scheduler ID on the worker side, which serves to
  // authenticate the scheduler in the common case of the the healthcheck's
  // runTask arriving before the next heartbeat.
  if (
    isHealthcheck
    && worker == worker_.id
    && rt.node == kHealthcheckTaskNewWorkerNode
    && scheduler != schedulerState_->id
  ) {
    LOG(INFO) << "Accepting a 'new worker' healthcheck from an unknown "
      << "scheduler, in the hopes that its heartbeat response is just about "
      << "to arrive.";
    // It may be reasonable to set *scheduler into schedulerState_->id here...
  } else {
    throwOnInstanceIDMismatch("runTask", scheduler, worker);
  }

  if (isHealthcheck) {
    LOG(INFO) << "Queueing healthcheck started at "
      << rt.invocationID.startTime;
  } else {
    LOG(INFO) << "Queueing task: " << debugString(rt);
  }

  SYNCHRONIZED_CONST(state_) { SYNCHRONIZED(runningTasks_) {
    // Don't run tasks sent before the most recent notifyIfTasksNotRunning,
    // except healthchecks, which don't use notifyIfTasksNotRunning anyway.
    if (
      !isHealthcheck && notify_if_tasks_not_running_sequence_num <
        state_.notifyIfTasksNotRunningSequenceNum_
    ) {
      throw BistroWorkerException(
        "Not running task ", debugString(rt), " since its "
        "notifyIfTasksNotRunning sequence number is ",
        notify_if_tasks_not_running_sequence_num, " < ",
        state_.notifyIfTasksNotRunningSequenceNum_
      );
    }

    // Mark the task as "running", if it was not already running.
    auto it_and_success = runningTasks_.emplace(TaskID{rt.job, rt.node}, rt);
    if (!it_and_success.second) {
      if (isHealthcheck) {
        // The scheduler doesn't track healtchecks, it might send duplicates.
        throw BistroWorkerException("Another healtcheck is already running.");
      }
      CHECK (it_and_success.first->second.invocationID != rt.invocationID)
        << "RunningTask was runTask'd more than once: " << debugString(rt);
      // This can happen if updateStatus succeeds on the scheduler, and
      // fails on the worker.  In this case, the scheduler might send the
      // next invocation of the same task before the next updateStatus retry.
      throw BistroWorkerException(
        "Tried to runTask when another invocation of this task is running: ",
        debugString(rt)
      );
    }
  } }

  timer.log("runTask setup was slow");

  taskQueue_.runTask(
    rt,
    cmd.empty() ? vector<string>{workerCommand_} : cmd,
    config,  // Job config argument -- DO: elide the extra copy?
    jobsDir_ / rt.job,  // Working directory for the task
    [this](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
      folly::AutoTimer<> timer("Task update queue was slow");
      timer.setMinTimeToLog(0.1);  // 10 tasks / sec
      notifyFinishedQueue_.blockingWrite(folly::make_unique<NotifyData>(
        TaskID{rt.job, rt.node}, std::move(status)
      ));
    }
  );
}

void BistroWorkerHandler::notifyIfTasksNotRunning(
    const std::vector<cpp2::RunningTask>& rts,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    int64_t notify_if_tasks_not_running_sequence_num) {

  throwOnInstanceIDMismatch("notifyIfTasksNotRunning", scheduler, worker);
  std::vector<cpp2::RunningTask> not_running_tasks;

  SYNCHRONIZED(state_) { SYNCHRONIZED_CONST(runningTasks_) {
    // Each time the scheduler retries, it bumps the counter.
    if (notify_if_tasks_not_running_sequence_num <=
          state_.notifyIfTasksNotRunningSequenceNum_) {
      throw BistroWorkerException(
        "Got out-of-order notifyIfTasksNotRunning ",
        notify_if_tasks_not_running_sequence_num, " <= ",
        state_.notifyIfTasksNotRunningSequenceNum_
      );
    }
    // After this point, we'll decline any runTask requests that were sent
    // before this notifyIfTasksNotRunning was sent.
    state_.notifyIfTasksNotRunningSequenceNum_ =
      notify_if_tasks_not_running_sequence_num;

    for (const auto& rt : rts) {
      auto it = runningTasks_.find(TaskID{rt.job, rt.node});
      if (it == runningTasks_.end()) {
        // Don't blockingWrite() here, since we are holding locks.
        not_running_tasks.push_back(rt);
      } else if (rt.invocationID != it->second.invocationID) {
        // This can happen, albeit rarely, if the query is much delayed, and
        // a new invocation of the same task is started up.
        LOG(WARNING) << "Got notifyIfTasksNotRunning query with mismatched "
          << "invocation ID: query " << debugString(rt) << " vs current "
          << debugString(it->second);
      }
      // Tasks that are running, whether or not their invocation ID
      // matches, will not generate a notification.
    }
  } }

  // Send out the status updates for the non-running tasks
  for (auto& rt : not_running_tasks) {
    folly::AutoTimer<> timer(
      "Queued 'was not running' update for ", debugString(rt)
    );
    notifyNotRunningQueue_.blockingWrite(std::move(rt));
  }
  if (not_running_tasks.empty()) {
    LOG(INFO) << "All " << rts.size()
      << " notifyIfTasksNotRunning tasks were running";
  }
}

void BistroWorkerHandler::requestSuicide(
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker) {

  throwOnInstanceIDMismatch("requestSuicide", scheduler, worker);
  LOG(WARNING) << "Scheduler requested suicide";
  suicide();
}

void BistroWorkerHandler::killTask(
    const cpp2::RunningTask& rt,
    cpp2::KilledTaskStatusFilter status_filter,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker) {

  throwOnInstanceIDMismatch("killTask", scheduler, worker);
  SYNCHRONIZED(runningTasks_) {
    auto it = runningTasks_.find(TaskID{rt.job, rt.node});
    if (it == runningTasks_.end()) {
      return;  // no such task, do nothing
    }
    if (it->second.invocationID != rt.invocationID) {
      throw BistroWorkerException(
        "Tried to kill one task invocation: ", debugString(rt),
        ", but a different one was running: ", debugString(it->second)
      );
    }
    // Don't erase rt from runningTasks_ here, which may cause races.
    // Let notifyFinished() clean it up in the background
  }

  // The kill can take a while, so don't hold any locks while we do it.
  // Runs the notification callback when it's done, just like normal exit.
  //
  // Crucially, this call does *not* touch runningTasks_ or state_, which
  // means that there are no interesting races with any of the other calls.
  taskQueue_.killTask(
    rt.job,
    rt.node,
    FLAGS_use_soft_kill ? cpp2::KillMethod::SOFT : cpp2::KillMethod::HARD,
    status_filter
  );
}

void BistroWorkerHandler::suicide() {
  LOG(WARNING) << "Committing suicide";
  _exit(1);
  // Rationale: As written, suicide is fast and fail-safe. A desirable
  // alternative would be to take a few seconds to soft-kill all the child
  // processes.  However, that would require the implementation to address a
  // variety of race conditions and issues:
  //
  //  - Ensure the "soft-kill" process takes a well-defined amount of time.
  //    This requires that we soft-kill many tasks in parallel, and
  //    potentially have a timer to hard-quit early.
  //
  //  - Prevent new tasks from being started.
  //
  //  - Decide what to do with notifyFinished(), notifyNotRunning() and with
  //    the unsent notifications.  In most cases, the scheduler won't want
  //    or will be unable to receive our notifications, so a poor
  //    implementation may generate a lot of 'failed notification' spam on
  //    the worker and/or scheduler.
  //
  //  - Avoid sending heartbeats. Avoid double-suicide.
  //
  //  - Take care with locks on state_ and runningTasks_ to avoid deadlocks
  //    and blocking for too long.
  //
  // Implementing all of the above correctly, and with tight control of
  // timing is a lot of work, and not clearly all that much better.  In
  // contrast, relying on PARENT_DEATH_SIGNAL is easy, and is good enough
  // for well-behaved children.  If it's crucial for suicide to do
  // soft-kills in your application, reopen the discussion.
}

void BistroWorkerHandler::throwOnInstanceIDMismatch(
    const string& func_name,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker) const {

  // Make a copy for a consistent error message
  auto scheduler_id = schedulerState_->id;
  if (scheduler == scheduler_id && worker == worker_.id) {
    return;
  }
  throw BistroWorkerException(
    "Worker ", worker_.id.startTime, "/" , worker_.id.rand, " with "
    "associated scheduler ", scheduler_id.startTime, "/", scheduler_id.rand,
    " ignored ", func_name, " for ", worker.startTime, "/", worker.rand,
    " from ", scheduler.startTime, "/", scheduler.rand
  );
}

chrono::seconds BistroWorkerHandler::notifyFinished() noexcept {
  // Reset the client every 100 calls. DO(agoder): why did you do that?
  shared_ptr<cpp2::BistroSchedulerAsyncClient> client;
  try {
    client = schedulerClientFn_(
      folly::EventBaseManager::get()->getEventBase()
    );
  } catch (const exception& e) {
    LOG(ERROR) << "notifyFinished: Unable to get client for scheduler";
    return chrono::seconds(5);
  }
  std::unique_ptr<NotifyData> nd;
  for (int i = 0; i < 100; ++i) {
    if (!notifyFinishedQueue_.read(nd)) {
      return chrono::seconds(1);
    }
    // Copy the RunningTask so that we don't have to lock runningTasks_
    // while we send the notification.
    cpp2::RunningTask rt;
    SYNCHRONIZED_CONST(runningTasks_) {
      auto task_it = runningTasks_.find(nd->taskID);
      CHECK(task_it != runningTasks_.end())
        << "Lost track of " << nd->taskID.first << "/" << nd->taskID.second;
      rt = task_it->second;
    }
    // Don't hold locks during this call, not just for better performance,
    // but also to avoid contention with e.g. notifyIfTasksNotRunning.
    try {
      auto scheduler_id = schedulerState_->id;  // Don't hold the lock
      client->sync_updateStatus(
        rt,
        // The scheduler would ignore the timestamp anyway.
        folly::toJson(nd->status.toDynamicNoTime()).toStdString(),
        scheduler_id,
        worker_.id
      );
    } catch (const exception& e) {
      LOG(ERROR) << "Unable to return status to scheduler: " << e.what();
      notifyFinishedQueue_.blockingWrite(std::move(nd));
      return chrono::seconds(1);
    }
    SYNCHRONIZED(runningTasks_) {
      CHECK(runningTasks_.erase(nd->taskID) == 1)
        << "Already removed " << nd->taskID.first << "/" << nd->taskID.second;
    }
    // Update internal health state to match the scheduler's
    if (rt.job == kHealthcheckTaskJob && nd->status.isDone()) {
      state_->timeLastGoodHealthcheckSent_ = rt.invocationID.startTime;
    }
  }
  return chrono::seconds(0);
}

/**
 * If the scheduler sees runTask fail, it uses notifyIfTasksNotRunning to
 * check whether it actually succeeded on the worker.  In case this was a
 * partial failure, the worker does not reply, and the scheduler assumes the
 * task is actually running.  Otherwise, the worker replies with a special
 * overwriteable "was not running" status to allow the scheduler to
 * reschedule the task.
 */
chrono::seconds BistroWorkerHandler::notifyNotRunning() noexcept {
  // Reset the client every 100 calls. DO(agoder): why did you do that?
  shared_ptr<cpp2::BistroSchedulerAsyncClient> client;
  try {
    client = schedulerClientFn_(
      folly::EventBaseManager::get()->getEventBase()
    );
  } catch (const exception& e) {
    LOG(ERROR) << "notifyNotRunning: Unable to get client for scheduler";
    return chrono::seconds(5);
  }
  cpp2::RunningTask rt;
  for (int i = 0; i < 100; ++i) {
    if (!notifyNotRunningQueue_.read(rt)) {
      return chrono::seconds(1);
    }
    try {
      auto scheduler_id = schedulerState_->id;  // Don't hold the lock
      client->sync_updateStatus(
        rt,
        // The scheduler would ignore the timestamp anyway.
        folly::toJson(TaskStatus::wasNotRunning().toDynamicNoTime())
          .toStdString(),
        scheduler_id,
        worker_.id
      );
    } catch (const exception& e) {
      LOG(ERROR) << "Cannot send non-running task to scheduler: " << e.what();
      notifyNotRunningQueue_.blockingWrite(std::move(rt));
      return chrono::seconds(1);
    }
  }
  return chrono::seconds(0);
}

chrono::seconds BistroWorkerHandler::heartbeat() noexcept {
  if (!canConnectToMyself_) {
    // Make a transient event base since we only use it for one sync call.
    apache::thrift::async::TEventBase evb;
    try {
      // Make a dummy request to myself to see if the server is up and
      // accessible via the external address to be used for heartbeats.
      cpp2::LogLines ignored;
      getAsyncClientForAddress<cpp2::BistroWorkerAsyncClient>(
        &evb,
        worker_.addr
      )->sync_getJobLogsByID(
        ignored, "statuses", {}, {}, 0, false, 1, ""
      );
      canConnectToMyself_ = true;
      // Fall through to sending a heartbeat
    } catch (const apache::thrift::TException& e) {
      LOG(WARNING) << "Waiting for this worker to start listening on "
        << debugString(worker_.addr) << ": " << e.what();
      return chrono::seconds(1);
    }
  }
  try {
    cpp2::SchedulerHeartbeatResponse res;
    schedulerClientFn_(
      folly::EventBaseManager::get()->getEventBase()
    )->sync_processHeartbeat(res, worker_);

    gotNewSchedulerInstance_ = schedulerState_->id != res.id;
    if (gotNewSchedulerInstance_) {
      LOG(INFO) << "Connected to new scheduler " << debugString(res);
    }

    SYNCHRONIZED(state_) {
      // The worker attempts to run exactly the same state / health model of
      // itself as the one run by the scheduler.  This way, it will stay
      // healthy (or up) for as exactly as long as long as the scheduler
      // wouldn't lose it.
      //
      // To avoid dealing with clock skew for the timeouts, they are all
      // computed in terms of worker time, which is accessed here and in
      // healthcheck() via time(null).  Note that the worker time is taken
      // before the request is sent, so it is logically earlier than the
      // "time heartbeat received" used by the scheduler.
      //
      // As a result, all timeouts on the worker expire just before they
      // would expire on the scheduler, which is the conservative approach
      // to the goal of not running tasks that shouldn't start.
      time_t cur_time = time(nullptr);
      if (gotNewSchedulerInstance_) {
        // The scheduler runs the same update logic in updateNewWorker when
        // a new worker connects.
        state_ = RemoteWorkerState(cur_time);
      }
      state_.timeLastHeartbeatReceived_ = cur_time;
      // Normally, this gets overwritten by the healthcheck() thread, but
      // this is the only way to get out of RemoteWorkerState::State::NEW.
      state_.state_ = RemoteWorkerState::State(res.workerState);

      // Update scheduler timeouts _inside_ the state_ lock, so that from
      // the point of view of healthcheck(), both the state & timeouts
      // change simultaneously.  As always, first lock state_, then
      // schedulerState_, and don't hold the latter lock.
      SYNCHRONIZED(schedulerState_) {
        schedulerState_ = res;
      }
    }
  } catch (const exception& e) {
    LOG(ERROR) << "Unable to send heartbeat to scheduler: " << e.what();
  }
  return chrono::seconds(worker_.heartbeatPeriodSec);
}

chrono::seconds BistroWorkerHandler::healthcheck() noexcept {
  try {
    time_t cur_time = time(nullptr);
    SYNCHRONIZED(state_) {
      // Lock state_ first, then schedulerState_ -- and don't hold this lock.
      auto scheduler_state = schedulerState_.copy();
      auto new_state = state_.computeState(
        cur_time,
        scheduler_state.maxHealthcheckGap,
        worker_.heartbeatPeriodSec + scheduler_state.heartbeatGracePeriod,
        scheduler_state.loseUnhealthyWorkerAfter
          // "Is the worker lost?" is checked periodically by the scheduler.
          // So, the scheduler may notice a lost worker some seconds later
          // than it should have been lost.  Since it's important for
          // workers to be lost in a timely fashion (to avoid duplicate
          // tasks, e.g.), the worker tries to die a bit sooner than the
          // scheduler would lose it.
          - scheduler_state.workerCheckInterval
      );
      if (new_state != RemoteWorkerState::State::HEALTHY
          && state_.state_ == RemoteWorkerState::State::HEALTHY) {
        LOG(WARNING) << "Became unhealthy";
        state_.timeBecameUnhealthy_ = cur_time;
      } else if (new_state == RemoteWorkerState::State::HEALTHY
                 && state_.state_ != RemoteWorkerState::State::HEALTHY) {
        LOG(INFO) << "Became healthy";
      }
      state_.state_ = new_state;
    }
    if (state_->state_ == RemoteWorkerState::State::MUST_DIE) {
      LOG(ERROR) << "Scheduler was about to lose this worker; quitting.";
      suicide();
    }
  } catch (const exception& e) {
    LOG(ERROR) << "Failed to update worker health state: " << e.what();
    suicide();
  }
  return chrono::seconds(1);  // This is cheap, so check often.
}

void BistroWorkerHandler::getJobLogsByID(
    cpp2::LogLines& out,
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    int limit,
    const string& regex_filter) {

  auto log = taskQueue_.getLogWriter()->getJobLogs(
    logtype,
    jobs,
    nodes,
    line_id,
    is_ascending,
    limit,
    regex_filter
  );

  out.nextLineID = log.nextLineID;
  for (const auto& l : log.lines) {
    out.lines.emplace_back(
      apache::thrift::FragileConstructor::FRAGILE,
      l.jobID, l.nodeID, l.time, l.line, l.lineID
    );
  }
}

}}
