/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/if/gen-cpp2/scheduler_types.h"
#include "bistro/bistro/physical/UsablePhysicalResourceMonitor.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/WorkerSetID.h"
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
  log_db_file_name, "task_logs.sql3",  // matches LocalRunner
  "One would rename the log DB when many workers use the same data_dir. "
  "Note that storing log DBs on a networked FS is guaranteed to reduce "
  "your overall system reliability."
);
DEFINE_int32(
  refresh_usable_physical_resources_sec, 60,
  "CGroups settings can change at runtime, altering our CPU or RAM "
  "allocation. nVidia GPUs can become lost. For both of these reasons, "
  "system resources are not static, but rather are polled periodically. "
  "This controls the refresh frequency."
);
DECLARE_int32(physical_resources_subprocess_timeout_ms);

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
    int32_t locked_port,
    BistroWorkerHandler::LogStateTransitionFn log_state_transition_fn) {

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
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  LOG(INFO) << "Worker is ready: " << debugString(worker);
  log_state_transition_fn("initializing", worker, nullptr);
  return worker;
}

}  // anonymous namespace

BistroWorkerHandler::BistroWorkerHandler(
    std::weak_ptr<apache::thrift::ThriftServer> server,
    const boost::filesystem::path& data_dir,
    LogStateTransitionFn log_state_transition_fn,
    SchedulerClientFn scheduler_client_fn,
    const string& worker_command,
    const cpp2::ServiceAddress& addr,
    int32_t locked_port)
  : fb303::FacebookBase2("BistroWorker"),
    logStateTransitionFn_(log_state_transition_fn),
    schedulerClientFn_(scheduler_client_fn),
    workerCommand_(worker_command),
    taskQueue_(std::make_unique<LogWriter>(
      data_dir / FLAGS_log_db_file_name
    )),
    notifyFinishedQueue_(100000),
    notifyNotRunningQueue_(10000),
    jobsDir_(data_dir / "jobs"),
    worker_(makeWorker(addr, locked_port, logStateTransitionFn_)),
    state_(RemoteWorkerState(worker_.id.startTime)),
    gotNewSchedulerInstance_(true),
    canConnectToMyself_(false),
    server_(std::move(server)) {

  // No scheduler associated yet, so use a dummy instance ID and timeouts
  schedulerState_->id.startTime = 0;
  schedulerState_->id.rand = 0;
  // Become unhealthy instantly
  schedulerState_->maxHealthcheckGap = 0;
  schedulerState_->heartbeatGracePeriod = 0;
  schedulerState_->workerCheckInterval = 0;
  // Default to needing 60 years to get lost, so we don't suicide on startup.
  schedulerState_->loseUnhealthyWorkerAfter = numeric_limits<int32_t>::max();
  schedulerState_->workerState = static_cast<int>(
    RemoteWorkerState::State::NEW  // not used
  );
  // ->workerSetID defaults to 'no workers' and a scheduler ID of 0/0, which
  // ensures that the scheduler perceives this set as belonging to a
  // different scheduler (take care to initialize your scheduler ID in unit
  // tests!), and that it cannot achieve consensus.

  // CAUTION: ThreadedRepeatingFunctionRunner recommends two-stage
  // initialization for starting threads.  This specific case is safe since:
  //  - this comes last in the constructor, so the class is fully constructed,
  //  - this class is final, so no derived classes remain to be constructed.
  backgroundThreads_.add(
    "BWH:healthcheck", bind(&BistroWorkerHandler::healthcheck, this)
  );
  backgroundThreads_.add(
    "BWH:heartbeat", bind(&BistroWorkerHandler::heartbeat, this)
  );
  backgroundThreads_.add(
    "BWH:ntfyFnshd", bind(&BistroWorkerHandler::notifyFinished, this)
  );
  backgroundThreads_.add(
    "BWH:ntfyNotRnng", bind(&BistroWorkerHandler::notifyNotRunning, this)
  );
}

BistroWorkerHandler::~BistroWorkerHandler() {
  // Block new requests, kill tasks, wait for them to exit, stop our server.
  killTasksAndStop();
  backgroundThreads_.stop();
}

void BistroWorkerHandler::throwIfSuicidal() {
  if (committingSuicide_.load()) {
    throw BistroWorkerException(
      "Worker ", worker_.id.startTime, "/" , worker_.id.rand,
      " is committing suicide"
    );
  }
}

// Design/protocol note:  If this is the first getRunningTasks(),
// state_->state_ will be changed from NEW by the scheduler's response to
// our next heartbeat.  This method does not mutate worker state, both
// because that is cleaner, and because that lets the scheduler query
// running tasks after worker intialization e.g. to poll resource usage.
void BistroWorkerHandler::getRunningTasks(
    std::vector<cpp2::RunningTask>& out_running_tasks,
    const cpp2::BistroInstanceID& worker) {

  throwIfSuicidal();

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
    int64_t notify_if_tasks_not_running_sequence_num,
    const cpp2::TaskSubprocessOptions& opts) {

  throwIfSuicidal();

  // 100 ms per log => 10 tasks/sec
  AutoTimer<> timer("runTask was slow", std::chrono::milliseconds{100});

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
    // This means that a worker will not know its usable physical resources
    // until it receives the first healthcheck, and that any cgroup config
    // changes (which should be extremely rare) will only propagate to
    // workers during healthchecks.  On the bright side, this means that
    // cgroup configs *can* be changed at runtime, and that the lock on
    // systemCGroupOpts_ causes a minimal amount of contention, since
    // healthchecks should not be too frequent.
    SYNCHRONIZED(usablePhysicalResources_) {
      // Future: Ideally, the refresh interval could also be changed at
      // run-time, and it would be possible to add a "log" callback to
      // display the new system resources whenever the cgroups change.
      // However, both are too much hassle with the current PeriodicPoller,
      // and it's too much hassle to roll a custom poller.
      if (!usablePhysicalResources_.monitor_) {
        LOG(WARNING) << "CGroups set: " << debugString(opts.cgroupOptions);
        usablePhysicalResources_.monitor_ =
          std::make_unique<UsablePhysicalResourceMonitor>(
            CGroupPaths(opts.cgroupOptions, folly::none),
            FLAGS_physical_resources_subprocess_timeout_ms,
            std::chrono::seconds(FLAGS_refresh_usable_physical_resources_sec)
          );
        usablePhysicalResources_.cgroupOpts_ = opts.cgroupOptions;
      } else if (opts.cgroupOptions != usablePhysicalResources_.cgroupOpts_) {
        LOG(WARNING) << "CGroups changed: " << debugString(opts.cgroupOptions);
        usablePhysicalResources_.monitor_->updateCGroupPaths(
          CGroupPaths(opts.cgroupOptions, folly::none)
        );
        usablePhysicalResources_.cgroupOpts_ = opts.cgroupOptions;
      }
    }
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
    [this](const cpp2::RunningTask& runningTask, TaskStatus&& status) noexcept {
      // 10 tasks / sec
      folly::AutoTimer<> updateQueueTimer(
          "Task update queue was slow", std::chrono::milliseconds{100});
      notifyFinishedQueue_.blockingWrite(std::make_unique<NotifyData>(
        TaskID{runningTask.job, runningTask.node}, std::move(status)
      ));
      logStateTransitionFn_("completed_task", worker_, &runningTask);
    },
    [this](
      const cpp2::RunningTask& runningTask, cpp2::TaskPhysicalResources&& res
    ) noexcept {
      SYNCHRONIZED(runningTasks_) {
        auto it = runningTasks_.find({runningTask.job, runningTask.node});
        CHECK (it != runningTasks_.end()) << "Bad task: "
                                          << debugString(runningTask);
        it->second.physicalResources_ref().value_unchecked() = std::move(res);
        it->second.__isset.physicalResources = true;
      }
    },
    opts
  );
  logStateTransitionFn_("queued_task", worker_, &rt);
}

void BistroWorkerHandler::notifyIfTasksNotRunning(
    const std::vector<cpp2::RunningTask>& rts,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    int64_t notify_if_tasks_not_running_sequence_num) {

  // Don't check committingSuicide_ since if we manage to reply to such
  // requests, it's overall better for consistency.

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
    folly::AutoTimer<> timer(folly::to<std::string>(
        "Queued 'was not running' update for ", debugString(rt)));
    notifyNotRunningQueue_.blockingWrite(std::move(rt));
  }
  if (not_running_tasks.empty()) {
    LOG(INFO) << "All " << rts.size()
      << " notifyIfTasksNotRunning tasks were running";
  }
}

// WATCH OUT: Unlike most Thrift calls, this CAN be called from threads not
// belonging to the ThriftServer (e.g. in test_worker).  This shouldn't
// change much, since the server could also be multithreaded.
void BistroWorkerHandler::requestSuicide(
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker) {

  throwIfSuicidal();  // We're already killing tasks, so bugger off
  throwOnInstanceIDMismatch("requestSuicide", scheduler, worker);
  LOG(WARNING) << "Scheduler requested suicide";
  logStateTransitionFn_("scheduler_requested_suicide", worker_, nullptr);
  // Block running tasks even before we return to the scheduler.
  committingSuicide_.store(true);
  // Don't wait until all tasks exit, so that the scheduler's request will
  // appear to succed quickly.
  folly::EventBaseManager::get()->getEventBase()->runInEventBaseThread(
    [this]() { killTasksAndStop(); }
  );
}

void BistroWorkerHandler::killTask(
    const cpp2::RunningTask& rt,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    const cpp2::KillRequest& req) {

  throwIfSuicidal();  // Technically, the kill will succeed, but ...
  throwOnInstanceIDMismatch("killTask", scheduler, worker);
  logStateTransitionFn_("kill_task_request", worker_, &rt);
  // Only sends the signal, does not wait -- if the task dies,
  // notifyFinished() will clean up.
  taskQueue_.kill(rt, req);  // Throws if the signal cannot be sent.
  LOG(INFO) << "Sent " << debugString(req) << " to " << debugString(rt);
  logStateTransitionFn_("kill_task_sent_signal", worker_, &rt);
}

void BistroWorkerHandler::killTasksAndStop() noexcept {
  committingSuicide_.store(true);
  LOG(WARNING) << "Trying to kill all tasks";
  logStateTransitionFn_("kill_all_tasks", worker_, nullptr);  // noexcept
  // While there are tasks, TERM-wait-KILL, rinse, and repeat.
  cpp2::KillRequest req;
  req.method = cpp2::KillMethod::TERM_WAIT_KILL;
  // req's timeout is set per-task.
  while (true) {
    size_t num_running_tasks = 0;
    SYNCHRONIZED(runningTasks_) {
      for (const auto& p : runningTasks_) {
        if (taskQueue_.isRunning(p.second)) {
          ++num_running_tasks;
        }
        req.killWaitMs = p.second.workerSuicideTaskKillWaitMs;
        // This can throw if the task had just exited, or if its
        // signal-processing is backlogged.
        try {
          taskQueue_.kill(p.second, req);
        } catch (const std::exception&) {}
      }
    }
    if (num_running_tasks == 0) {
      break;
    }
    /*sleep override*/ std::this_thread::sleep_for(
      std::chrono::milliseconds(20)  // check 20 times per second
    );
  }
  LOG(WARNING) << "Committing suicide";
  logStateTransitionFn_("suicide", worker_, nullptr);  // noexcept
  if (auto server = server_.lock()) {
    try {
      server->stop();
    } catch (const std::exception& ex) {
      // Unlikely, but we'd better catch it since this runs in a destructor.
      LOG(ERROR) << "ThriftServer::stop() threw: " << ex.what();
    }
  }
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

std::chrono::seconds BistroWorkerHandler::notifyFinished() noexcept {
  // Not checking commitedSuicide_ here since, in rare cases, some good can
  // come out of these notifications (the tasks are already done, so we
  // might as well try to report them if the scheduler will listen).

  // Reset the client every 100 calls. DO(agoder): why did you do that?
  shared_ptr<cpp2::BistroSchedulerAsyncClient> client;
  try {
    client = schedulerClientFn_(
      folly::EventBaseManager::get()->getEventBase()
    );
  } catch (const exception& e) {
    LOG(ERROR) << "notifyFinished: Unable to get client for scheduler";
    return std::chrono::seconds(5);
  }
  std::unique_ptr<NotifyData> nd;
  for (int i = 0; i < 100; ++i) {
    if (!notifyFinishedQueue_.read(nd)) {
      return std::chrono::seconds(1);
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
        folly::toJson(nd->status.toDynamicNoTime()),
        scheduler_id,
        worker_.id
      );
    } catch (const exception& e) {
      logStateTransitionFn_("scheduler_failed_to_acknowledge", worker_, &rt);
      LOG(ERROR) << "Unable to return status to scheduler: " << e.what();
      notifyFinishedQueue_.blockingWrite(std::move(nd));
      return std::chrono::seconds(1);
    }
    SYNCHRONIZED(runningTasks_) {
      CHECK(runningTasks_.erase(nd->taskID) == 1)
        << "Already removed " << nd->taskID.first << "/" << nd->taskID.second;
    }
    // Update internal health state to match the scheduler's
    if (rt.job == kHealthcheckTaskJob && nd->status.isDone()) {
      state_->timeLastGoodHealthcheckSent_ = rt.invocationID.startTime;
    }
    logStateTransitionFn_("acknowledged_by_scheduler", worker_, &rt);
  }
  return std::chrono::seconds(0);
}

/**
 * If the scheduler sees runTask fail, it uses notifyIfTasksNotRunning to
 * check whether it actually succeeded on the worker.  In case this was a
 * partial failure, the worker does not reply, and the scheduler assumes the
 * task is actually running.  Otherwise, the worker replies with a special
 * overwriteable "was not running" status to allow the scheduler to
 * reschedule the task.
 */
std::chrono::seconds BistroWorkerHandler::notifyNotRunning() noexcept {
  // Just as with notifyFinished, there is no benefit to checking
  // committingSuicide_ here.

  // Reset the client every 100 calls. DO(agoder): why did you do that?
  shared_ptr<cpp2::BistroSchedulerAsyncClient> client;
  try {
    client = schedulerClientFn_(
      folly::EventBaseManager::get()->getEventBase()
    );
  } catch (const exception& e) {
    LOG(ERROR) << "notifyNotRunning: Unable to get client for scheduler";
    return std::chrono::seconds(5);
  }
  cpp2::RunningTask rt;
  for (int i = 0; i < 100; ++i) {
    if (!notifyNotRunningQueue_.read(rt)) {
      return std::chrono::seconds(1);
    }
    try {
      auto scheduler_id = schedulerState_->id;  // Don't hold the lock
      client->sync_updateStatus(
        rt,
        // The scheduler would ignore the timestamp anyway.
        folly::toJson(TaskStatus::wasNotRunning().toDynamicNoTime()),
        scheduler_id,
        worker_.id
      );
    } catch (const exception& e) {
      LOG(ERROR) << "Cannot send non-running task to scheduler: " << e.what();
      notifyNotRunningQueue_.blockingWrite(std::move(rt));
      return std::chrono::seconds(1);
    }
  }
  return std::chrono::seconds(0);
}

void BistroWorkerHandler::setState(
    RemoteWorkerState* state,
    RemoteWorkerState::State new_state,
    time_t cur_time) {
  // Future: this is inside a Synchronized lock, so LOG() is very costly.
  if (new_state != RemoteWorkerState::State::HEALTHY
      && state->state_ == RemoteWorkerState::State::HEALTHY) {
    LOG(WARNING) << "Became unhealthy";
    state->timeBecameUnhealthy_ = cur_time;
    logStateTransitionFn_("became_unhealthy", worker_, nullptr);
  } else if (new_state == RemoteWorkerState::State::HEALTHY
             && state->state_ != RemoteWorkerState::State::HEALTHY) {
    LOG(INFO) << "Became healthy";
    logStateTransitionFn_("became_healthy", worker_, nullptr);
  }
  state->state_ = new_state;
  // Mirrors RemoteWorker::setState. This is how workers become HEALTHY for
  // the first time, since they lacks `consensus_permits_becoming_healthy`.
  state->hasBeenHealthy_ = state->hasBeenHealthy_
    || (new_state == RemoteWorkerState::State::HEALTHY);
}

std::chrono::seconds BistroWorkerHandler::heartbeat() noexcept {
  if (committingSuicide_.load()) {  // Stop sending heartbeats once dying.
    return std::chrono::seconds(1);
  }
  if (!canConnectToMyself_) {
    // Make a transient event base since we only use it for one sync call.
    EventBase evb;
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
      return std::chrono::seconds(1);
    }
    logStateTransitionFn_("listening", worker_, nullptr);
  }
  try {
    cpp2::SchedulerHeartbeatResponse res;
    // Create a copy of worker and update system resources
    cpp2::BistroWorker worker(worker_);
    SYNCHRONIZED(usablePhysicalResources_) {
      // The monitor is created during the first healthcheck.
      if (usablePhysicalResources_.monitor_) {
        try {
          worker.usableResources =
            *usablePhysicalResources_.monitor_->getDataOrThrow();
        } catch (const std::exception& ex) {
          LOG(WARNING) << "Failed to refresh worker's usable physical "
            << "resources: " << ex.what();
        }
      }
    }
    auto scheduler_state = schedulerState_.copy();  // Take the lock only once
    schedulerClientFn_(
      folly::EventBaseManager::get()->getEventBase()
    )->sync_processHeartbeat(res, worker, scheduler_state.workerSetID);
    enforceWorkerSchedulerProtocolVersion(
      worker_.protocolVersion, res.protocolVersion
    );
    CHECK(res.workerSetID.schedulerID == res.id);

    gotNewSchedulerInstance_ = scheduler_state.id != res.id;
    if (gotNewSchedulerInstance_) {
      LOG(INFO) << "Connected to new scheduler " << debugString(res);
      logStateTransitionFn_("connected_to_new_scheduler", worker_, nullptr);
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
      // Normally, the healthcheck thread overwrites state_.state_, but this
      // is the only way to get out of RemoteWorkerState::State::NEW.
      setState(&state_, RemoteWorkerState::State(res.workerState), cur_time);

      // Don't allow the worker set version to go backwards. NB: It could be
      // better to also ignore other scheduler state, but this version is
      // updated too rarely to be a useful sequence number.
      if (!gotNewSchedulerInstance_ && WorkerSetIDEarlierThan()(
        res.workerSetID.version, scheduler_state.workerSetID.version
      )) {
        LOG(ERROR) << "Got scheduler response with older WorkerSetID "
          << "version, not updating workerSetID -- current: "
          << debugString(scheduler_state.workerSetID) << ", received: "
          << debugString(res.workerSetID);
        // Holding back the workerSetID cannot cause schedulerState_.id and
        // schedulerState_.workerSetID.schedulerID to diverge.
        CHECK(scheduler_state.workerSetID.schedulerID == res.id);
        res.workerSetID = scheduler_state.workerSetID;
      }

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
    logStateTransitionFn_("error_sending_heartbeat", worker_, nullptr);
  }
  return std::chrono::seconds(worker_.heartbeatPeriodSec);
}

std::chrono::seconds BistroWorkerHandler::healthcheck() noexcept {
  if (committingSuicide_.load()) {  // No point in updating state_ any more.
    return std::chrono::seconds(1);
  }
  try {
    time_t cur_time = time(nullptr);
    SYNCHRONIZED(state_) {
      // Lock state_ first, then schedulerState_ -- and don't hold this lock.
      auto scheduler_state = schedulerState_.copy();
      auto new_state_and_disallowed = state_.computeState(
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
          - scheduler_state.workerCheckInterval,
        // The worker never "transiently" allows itself to be healthy.
        // Instead, this state transition happens when a heartbeat response
        // updates state_->hasBeenHealthy_.
        false
      );
      if (new_state_and_disallowed.second) {
        LOG(INFO) << "Will be healthy upon achieving WorkerSetID consensus.";
      }
      setState(&state_, new_state_and_disallowed.first, cur_time);
    }
    if (state_->state_ == RemoteWorkerState::State::MUST_DIE) {
      LOG(ERROR) << "Scheduler was about to lose this worker; quitting.";
      logStateTransitionFn_("suicide_unhealthy_too_long", worker_, nullptr);
      killTasksAndStop();
    }
  } catch (const exception& e) {  // This could have been a CHECK...
    LOG(ERROR) << "Failed to update worker health state: " << e.what();
    logStateTransitionFn_("health_checker_bug", worker_, nullptr);
    killTasksAndStop();
  }
  return std::chrono::seconds(1);  // This is cheap, so check often.
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

  // Even if we are committingSuicide_, there's no harm in returning logs.

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
