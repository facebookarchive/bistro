/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/runners/RemoteWorkerRunner.h"

#include <folly/experimental/AutoTimer.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkers.h"
#include "bistro/bistro/remote/RemoteWorkerSelector.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/utils/service_clients.h"
#include "bistro/bistro/if/gen-cpp2/BistroWorker_custom_protocol.h"
#include "bistro/bistro/flags/Flags.h"

DEFINE_int32(
  CAUTION_startup_wait_for_workers, -1,
  "At startup, the scheduler has to wait for workers to connect, so that "
  "we do not accidentally re-start tasks that are already running elsewhere. "
  "The default of -1 computes a 'minimum safe wait' from your healthcheck "
  "and worker loss timeouts. CAUTION: If you reduce these timeouts from one "
  "scheduler run to the next, the new default wait may not be long enough. "
);

namespace facebook { namespace bistro {

using namespace std;
using namespace apache::thrift::async;
using namespace apache::thrift;

RemoteWorkerRunner::RemoteWorkerRunner(
    shared_ptr<TaskStatuses> task_statuses,
    std::shared_ptr<Monitor> monitor)
  : workerLevel_(StringTable::NotFound),
    taskStatuses_(task_statuses),
    eventBase_(new TEventBase()),
    eventBaseThread_(bind(&TEventBase::loopForever, eventBase_.get())),
    inInitialWait_(true),
    startTime_(time(nullptr)),
    monitor_(monitor) {

  // Monitor the workers: send healthchecks, mark them (un)healthy / lost, etc
  runInBackgroundLoop([this](){
    RemoteWorkerUpdate update;
    workers_->updateState(&update);
    checkInitialWait(update);  // Must be called **after** updateState
    applyUpdate(&update);
    return chrono::seconds(RemoteWorkerState::workerCheckInterval());
  });
}

RemoteWorkerRunner::~RemoteWorkerRunner() {
  stopBackgroundThreads();
  eventBase_->terminateLoopSoon();
  eventBaseThread_.join();
}

void RemoteWorkerRunner::updateConfig(std::shared_ptr<const Config> config) {
  folly::AutoTimer<> timer;
  DEFINE_MONITOR_ERROR(monitor_, error, "RemoteWorkerRunner resource update");

  // Memoize these two values to be used by the next runTask
  workerLevel_ = config->levels.lookup("worker");
  CHECK(workerLevel_ != config->levels.NotFound);
  config_ = config;

  const auto& resources = config->resourcesByLevel[workerLevel_];
  // Always lock workerResources_ first, then workers_
  SYNCHRONIZED(workerResources_) { SYNCHRONIZED_CONST(workers_) {
    workerResources_.clear();
    // While it __looks__ like we no longer need 'workers' to be locked
    // after this loop, that lock is actually simultaneously ensuring that
    // RemoteWorkerRunner::runTaskImpl() gets to mark the task running in
    // the scheduler *BEFORE* the next updateWorkerResources().  So, there
    // would be no savings by releasing the lock before we are done here.
    for (const auto& wconn : workers_) {
      const auto& w = wconn.second->getBistroWorker();
      // Try for hostport, and fallback to hostname
      auto it = config->workerResourcesOverride.find(
        folly::to<string>(w.machineLock.hostname, ':', w.machineLock.port)
      );
      if (it == config->workerResourcesOverride.end()) {
        it = config->workerResourcesOverride.find(w.machineLock.hostname);
      }
      if (it == config->workerResourcesOverride.end()) {
        workerResources_[w.shard] = resources;
      } else {
        workerResources_[w.shard] = it->second;
      }
    }

    // Use the scheduler's view of running tasks, because that's consistent
    // with the other information we're using (while the running tasks in
    // RemoteWorkers might be more up-to-date).
    //
    // This is a moral copy of the corresponding loop in Scheduler. It will
    // disappear as soon as workers become nodes.
    auto running_tasks = taskStatuses_->copyRunningTasks();
    for (const auto& id_and_task : running_tasks) {
      const auto& rt = id_and_task.second;
      for (const auto& nr : rt.nodeResources) {
        if (nr.node != rt.workerShard) {
          continue;  // These are tracked in Scheduler
        }
        auto it = workerResources_.find(rt.workerShard);
        if (it == workerResources_.end()) {
          LOG(ERROR) << error.report(
            "Resources for unknown worker ", rt.workerShard, " from ",
            debugString(rt)
          );
          break;  // cannot use this running task's resources
        }
        auto& resources = it->second;
        for (const auto r : nr.resources) {
          auto rid = config->resourceNames.lookup(r.name);
          if (rid == StringTable::NotFound || rid >= resources.size()) {
            LOG(ERROR) << error.report(
              "Resource ", r.name, "/", rid, " not valid or known for worker ",
              rt.workerShard, ": ", debugString(rt)
            );
            continue;
          }
          resources[rid] -= r.amount;
          if (resources[rid] < 0) {
            LOG(ERROR) << error.report(
              "Resource ", r.name, " is ", resources[rid], " on worker ",
              rt.workerShard, " for ", debugString(rt)
            );
          }
        }
      }
    }
  }}

  if (FLAGS_log_performance) {
    timer.log("Updated RemoteWorkerRunner resources");
  }
}

LogLines RemoteWorkerRunner::getJobLogs(
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    const string& regex_filter) {

  LogLines res;
  res.nextLineID = LogLine::kNotALineID;
  time_t cur_time = time(nullptr);  // timestamp for any errors

  // We are going to query all the workers. This is wasteful, but it makes
  // it much easier to find logs for tasks, because:
  //  1) Multiple workers can have logs for different iterations of a task
  //  2) The logs API supports multi-queries, which, in some cases,
  //     require us to query all workers anyhow.
  std::vector<cpp2::ServiceAddress> services;
  std::vector<std::string> unhealthy_workers;
  std::vector<std::string> lost_workers;
  SYNCHRONIZED_CONST(workers_) {
    for (const auto& wconn : workers_) {
      const auto& w = wconn.second->getBistroWorker();
      // Instead of trying to fetch logs from unhealthy workers, which can
      // be slow, and degrade the user experience, display a "transient"
      // error right away.
      auto state = wconn.second->getState();
      if (state == RemoteWorkerState::State::UNHEALTHY) {
        unhealthy_workers.push_back(w.shard);
      } else if (state == RemoteWorkerState::State::MUST_DIE) {
        lost_workers.push_back(w.shard);
      } else {
        services.push_back(w.addr);
      }
    }
  }

  // Inform the user about the logs that we are not querying.
  std::string unqueried_workers;
  if (!unhealthy_workers.empty()) {
    unqueried_workers += "unhealthy: " + folly::join(", ", unhealthy_workers);
  }
  if (!lost_workers.empty()) {
    if (!unqueried_workers.empty()) {
      unqueried_workers += "; ";
    }
    unqueried_workers += "lost: " + folly::join(", ", lost_workers);
  }

  if (services.empty()) {
    if (unqueried_workers.empty()) {
      throw BistroException("No workers connected; cannot query logs.");
    } else {
      throw BistroException(
        "All workers are unhealthy; cannot query logs. Known workers: ",
        unqueried_workers
      );
    }
  }

  if (!unqueried_workers.empty()) {
    auto err = folly::to<string>(
      "Warning: some workers are unhealthy, so we cannot return logs from "
      "them. If your task ever ran on any of the following workers, these "
      "results may be incomplete -- ", unqueried_workers
    );
    LOG(WARNING) << err;
    res.lines.emplace_back("", "", cur_time, err, LogLine::kNotALineID);
  }

  // Use this thread's EventBase so that we can loopForever() to wait.
  auto* event_base =
    apache::thrift::async::TEventBaseManager::get()->getEventBase();

  // Query all the workers for their logs
  fanOutRequestToServices<cpp2::BistroWorkerAsyncClient, cpp2::LogLines>(
    event_base,
    resolve_callback(&cpp2::BistroWorkerAsyncClient::getJobLogsByID),
    &cpp2::BistroWorkerAsyncClient::recv_getJobLogsByID,
    // Handle exceptions for individual hosts by inserting "error" log lines
    // into the output, so that the client still gets some partial results.
    [&res, cur_time](exception_ptr ex, const string& service_id) {
      try {
        std::rethrow_exception(ex);  // ex is guaranteed to be non-null
      } catch(const std::exception& e) {
        auto err = folly::to<string>(
          "Failed to fetch logs from worker ", service_id, ", this may be ",
          "transient -- but if it is not, report it: ", e.what()
        );
        LOG(ERROR) << err;
        res.lines.emplace_back("", "", cur_time, err, LogLine::kNotALineID);
      }
    },
    // FinishFunc: a callback for when all workers have been processed.
    [is_ascending, line_id, &res](
      vector<unique_ptr<cpp2::LogLines>>&& results,
      apache::thrift::async::TEventBase* evb
    ) {
      // Find the most restrictive nextLineID among all hosts
      for (const auto& log : results) {
        if (
          // This value marks "no more lines" in LogWriter, so exclude it.
          (log->nextLineID != LogLine::kNotALineID) && (
            // This line only matches initially due to the previous comparison
            (res.nextLineID == LogLine::kNotALineID) ||
            ((res.nextLineID > log->nextLineID) == is_ascending)
          )
        ) {
          res.nextLineID = log->nextLineID;
        }
      }
      // Merge log lines, dropping those beyond the selected nextLineID.
      //
      // This means that the client is guaranteed log lines that are truly
      // sequential in line_id, making for an API that's easy to use
      // correctly.  In principle, this filtering could be done on the
      // client-side, and the non-sequential lines could be displayed
      // separately, but this seems too confusing for casual users.
      for (const auto& log : results) {
        for (const cpp2::LogLine& l : log->lines) {
          if (
            // If we are on the last page of results on all hosts, drop nothing.
            (res.nextLineID == LogLine::kNotALineID) ||
            ((l.lineID < res.nextLineID) == is_ascending)
          ) {
            res.lines.emplace_back(
              l.jobID, l.nodeID, l.time, l.line, l.lineID
            );
          }
        }
      }
      evb->terminateLoopSoon();
    },
    services,
    logtype,
    jobs,
    nodes,
    line_id,
    is_ascending,
    // at least 10 lines per worker, but try to stay under 5000 lines total
    max(5000 / (int)services.size(), 10),
    regex_filter
  );

  event_base->loopForever();  // Execute scheduled work, and wait for finish_fn

  // Sort the merged results from all the workers
  sort(
    res.lines.begin(),
    res.lines.end(),
    [is_ascending](const LogLine& a, const LogLine& b) {
      return (!is_ascending) ^ (a.lineID < b.lineID);
    }
  );

  return res;
}

cpp2::SchedulerHeartbeatResponse RemoteWorkerRunner::processWorkerHeartbeat(
    const cpp2::BistroWorker& worker,
    RemoteWorkerUpdate update) {

  auto r = workers_->processHeartbeat(&update, worker);
  // This will often result in the healthcheck or getRunningTasks arriving
  // before the heartbeat response does (meaning the worker does not yet
  // know the scheduler).  As a result, "new worker" healthchecks are
  // special-cased in BistroWorkerHandler::runTask().
  applyUpdate(&update);
  if (!r.hasValue()) {
    // This is our response to workers that are not accepted as current.
    //
    // TODO(#5023846): Replacing this by a specific error would create
    // another opportunity to solve this task -- the worker could decide to
    // commit suicide simply on account of its heartbeat being rejected.
    throw std::runtime_error("Worker shouldn't associate with this scheduler");
  }
  r->id = schedulerID_;
  return r.value();
}

// If this throws, the remote worker will re-send the status update.
void RemoteWorkerRunner::remoteUpdateStatus(
    const cpp2::RunningTask& rt,
    TaskStatus&& status,
    const cpp2::BistroInstanceID scheduler_id,
    const cpp2::BistroInstanceID worker_id) {

  if (scheduler_id != schedulerID_) {
    throw BistroException(
      "Got a status for ", rt.job, ", ", rt.node, " from a different ",
      "scheduler. My ID: ", schedulerID_.startTime, ", ", schedulerID_.rand,
      ", received ID: ", scheduler_id.startTime, ", ", scheduler_id.rand
    );
  }
  SYNCHRONIZED(workers_) {
    // Contract of RemoteWorker::recordNonRunningTaskStatus: Throws if the
    // scheduler or worker ID does not match, or if an update comes from a
    // NEW worker.  This causes the remote worker to re-send the update
    // later.  Logs & returns false on updates for running tasks with wrong
    // invocation IDs.  Records health-check replies, and returns false.
    // Updates the RemoteWorker's internal "running" and "unsure if running"
    // task lists.
    if (workers_.mutableWorkerOrThrow(rt.workerShard)
          ->recordNonRunningTaskStatus(rt, status, worker_id)) {
      // IMPORTANT: Update TaskStatuses **inside** the workers_ lock
      taskStatuses_->updateStatus(rt, std::move(status));
    }
  }
}

TaskRunnerResponse RemoteWorkerRunner::runTaskImpl(
  const std::shared_ptr<const Job>& job,
  const std::shared_ptr<const Node>& node,
  cpp2::RunningTask& rt,
  folly::dynamic& job_args,
  function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  if (inInitialWait_.load(std::memory_order_relaxed)) {
    return DoNotRunMoreTasks;
  }

  // Make a copy so we don't have to lock workers_ while waiting for Thrift.
  cpp2::BistroWorker worker;
  // unused initialization to hush a Clang warning:
  int64_t did_not_run_sequence_num = 0;
  // Always lock workerResources_ first, then workers_
  SYNCHRONIZED(workerResources_) {
    SYNCHRONIZED(workers_) {
      CHECK(workerLevel_ != StringTable::NotFound)
        << "updateConfig must always be invoked before runTask";
      // Whichever selector is currently in use is protected by the locks on
      // WorkerResources and RemoteWorkers, so no extra mutex is needed.
      auto response = RemoteWorkerSelector::getSingleton(
        // Since we use a memoized config, the selector type might be a bit
        // stale, but that's no big deal.  We need the stale config anyway,
        // since its worker level ID is the correct index into
        // workerResources_.
        config_->remoteWorkerSelectorType
      )->findWorker(
        *job,
        *node,
        workerLevel_,  // Needed for checking worker-level job filters.
        monitor_.get(),
        &workerResources_,
        &workers_,
        &worker,
        &did_not_run_sequence_num
      );
      if (response != RanTask) {
        return response;
      }
      rt.workerShard = worker.shard;
      job_args["worker_node"] = rt.workerShard;
      job_args["worker_host"] = worker.machineLock.hostname;

      // Add worker resources to rt **before** recording this task as running.
      auto& resources_by_node = job_args["resources_by_node"];
      CHECK(resources_by_node.find(rt.workerShard)
            == resources_by_node.items().end())
        << "Cannot have both a node and a worker named: " << rt.workerShard
        << " -- if you are running a worker and the central scheduler on the "
        << "same host, you should specify --instance_node_name global.";
      addNodeResourcesToRunningTask(
        &rt,
        &resources_by_node,
        // This config is stale, but it's consistent with workerResources_.
        *config_,
        rt.workerShard,
        workerLevel_,
        job->resources()
      );

      // Mark the task 'running' before we unlock workerResources_, because
      // otherwise an intervening updateConfig() could free the resources we
      // just grabbed.
      auto status = TaskStatus::running(
        // Pass some additional metadata to TaskStatusObservers that log
        make_shared<folly::dynamic>(folly::dynamic::object
          ("shard", worker.shard)
          ("hostname", worker.machineLock.hostname)
          ("port", worker.machineLock.port)
        )
      );
      workers_.mutableWorkerOrAbort(worker.shard)
        ->recordRunningTaskStatus(rt, status);
      // IMPORTANT: Update TaskStatuses **inside** the workers_ lock
      cb(rt, std::move(status));
    }
  }

  // Now we have a healthy worker with resources we already grabbed. It may
  // get marked unhealthy or even lost, since workers_ is now unlocked, but
  // we just have to try our luck.
  eventBase_->runInEventBaseThread([
      cb, this, rt, job_args, worker, did_not_run_sequence_num]() noexcept {

    try {
      shared_ptr<cpp2::BistroWorkerAsyncClient>
        client{getWorkerClient(worker)};
      client->runTask(
        unique_ptr<RequestCallback>(new FunctionReplyCallback(
          [this, cb, client, rt, worker, did_not_run_sequence_num](
              ClientReceiveState&& state) noexcept {

            // TODO(#5025478): Convert this, and the other recv_* calls in
            // this file to use recv_wrapped_*.
            try {
              client->recv_runTask(state);
            } catch (const cpp2::BistroWorkerException& e) {
              LOG(ERROR) << "Worker never started task: " << e.message;
              // Okay to mark the task "not running" since we know for sure
              // that the worker received & processed our request, and
              // decided not to run it.
              SYNCHRONIZED(workers_) {
                auto status = TaskStatus::neverStarted(e.message);
                workers_.mutableWorkerOrAbort(worker.shard)
                  ->recordFailedTask(rt, status);
                // IMPORTANT: Update TaskStatuses **inside** the workers_ lock
                cb(rt, std::move(status));
              }
            } catch (const std::exception& e) {
              // The task may or may not be running, so do NOT invoke the
              // callback, or the scheduler might schedule duplicate tasks,
              // exceed resource limits, etc.
              LOG(ERROR) << "The runTask request hit an error, will have to "
                "poll to find if the task is running: " << e.what();
              SYNCHRONIZED(workers_) {
                workers_.mutableWorkerOrAbort(worker.shard)
                  ->addUnsureIfRunningTask(rt);
              }
            }
          }
        )),
        rt,
        folly::toJson(job_args).toStdString(),
        vector<string>{},  // Use the default worker command
        schedulerID_,
        worker.id,
        // The real sequence number may have been incremented after we found
        // the worker, but this is okay, the worker simply rejects the task.
        did_not_run_sequence_num
      );
    } catch (const exception& e) {
      // We can get here if client creation failed (e.g. TAsyncSocket could
      // not resolve the hostname), or if the runTask request creation
      // failed.  The latter can __probably__ only happen if the connection
      // is dead at the time of the request.
      //
      // The key part is that in both cases, it ought to be true that the
      // request never reached the worker, and since the task is not
      // running, we can invoke the callback.
      LOG(ERROR) << "Error connecting to the worker: " << e.what();
      SYNCHRONIZED(workers_) {
        auto status = TaskStatus::neverStarted(e.what());
        workers_.mutableWorkerOrAbort(worker.shard)
          ->recordFailedTask(rt, status);
        // IMPORTANT: Update TaskStatuses **inside** the workers_ lock
        cb(rt, std::move(status));
      }
    }
  });
  return RanTask;
}

// Must be called after "update" is populated by updateState.
void RemoteWorkerRunner::checkInitialWait(const RemoteWorkerUpdate& update) {
  DEFINE_MONITOR_ERROR(monitor_, error, "RemoteWorkerRunner initial wait");
  if (!inInitialWait_.load(std::memory_order_relaxed)) {
    return;  // Clear the "intial wait" errors from the UI.
  }

  time_t min_safe_wait =
    RemoteWorkerState::maxHealthcheckGap() +
    RemoteWorkerState::loseUnhealthyWorkerAfter() +
    RemoteWorkerState::workerCheckInterval();  // extra safety gap
  time_t min_start_time = time(nullptr);
  if (FLAGS_CAUTION_startup_wait_for_workers < 0) {
    min_start_time -= min_safe_wait;
  } else {
    min_start_time -= FLAGS_CAUTION_startup_wait_for_workers;
    if (RemoteWorkerState::maxHealthcheckGap()
        > FLAGS_CAUTION_startup_wait_for_workers) {
      LOG(ERROR) << error.report(
        "DANGER! DANGER! Your --CAUTION_startup_wait_for_workers ",
        "of ", FLAGS_CAUTION_startup_wait_for_workers,
        " is lower than the max healthcheck gap of ",
        RemoteWorkerState::maxHealthcheckGap(), ", which makes it very ",
        "likely that you will start second copies of tasks that are ",
        "already running (unless your heartbeat interval is much smaller) "
      );
    } else if (min_safe_wait > FLAGS_CAUTION_startup_wait_for_workers) {
      LOG(WARNING) << error.report(
        "Your custom --CAUTION_startup_wait_for_workers is ",
        "less than the minimum safe value of ", min_safe_wait,
        " -- this increases the risk of starting second copies of tasks ",
        "that were already running."
      );
    }
  }

  // If, after the initial wait time expired, we are still querying running
  // tasks, then the scheduler may have restarted, and one of the workers,
  // while slow, is running tasks we do not know about.  To be safe, stay in
  // initial wait until all getRunningTasks succeed.
  if (min_start_time < startTime_) {
    LOG(INFO) << error.report("Waiting for all workers to connect");
  } else if (update.newWorkers().empty()) {
    inInitialWait_.store(false, std::memory_order_relaxed);
  } else {
    LOG(ERROR) << error.report(
      "Initial wait time expired, but not all workers' running tasks were "
      "fetched; not allowing tasks to start until all are fetched."
    );
  }
}

///
/// Thrift helpers
///

shared_ptr<cpp2::BistroWorkerAsyncClient> RemoteWorkerRunner::getWorkerClient(
    const cpp2::BistroWorker& w) {
  return getAsyncClientForAddress<cpp2::BistroWorkerAsyncClient>(
    eventBase_.get(), w.addr
  );
}

// We deliberately don't track healthchecks that we send. This is a tiny bit
// faster, and prevents them from being shown in the UI.  To match,
// RemoteWorker::updateTaskStatus does not call updateRunningTasks.
void RemoteWorkerRunner::sendWorkerHealthcheck(
    const cpp2::BistroWorker& w, bool is_new_worker) noexcept {

  eventBase_->runInEventBaseThread([this, w, is_new_worker]() noexcept {
    try {
      shared_ptr<cpp2::BistroWorkerAsyncClient> client{getWorkerClient(w)};
      // Make a barebones RunningTask; only job, startTime & shard are be used.
      cpp2::RunningTask rt;
      rt.job = kHealthcheckTaskJob;
      // BistroWorkerHandler::runTask doesn't check the scheduler ID for new
      // worker healthchecks.
      rt.node = is_new_worker ? kHealthcheckTaskNewWorkerNode : "";
      rt.invocationID.startTime = time(nullptr);
      // .rand is ignored for healthchecks
      rt.workerShard = w.shard;
      client->runTask(
        unique_ptr<RequestCallback>(new FunctionReplyCallback(
          [client, w](ClientReceiveState&& state) {
            try {
              client->recv_runTask(state);
            } catch (const exception& e) {
              // Don't track 'unsure if running' errors for healthchecks
              // because they have protection against replays, and there's
              // no harm in missing a few.
              //
              // DO: This and the below error could be reported to the
              // monitor on a per-worker basis, but the trick would be to
              // clear the message once the worker is lost / "must die".  An
              // alternative would be to make "errors with expiration",
              // which could allow the reporting of RemoteWorker logs too.
              LOG(ERROR) << "Error health-checking " << debugString(w)
                << ": " << e.what();
            }
          }
        )),
        rt,
        "", // No task config (3rd command-line argument)
        // Short script that prints "done" to our status pipe. The last
        // argument becomes $0 for the shell, making it look nicer in "ps".
        vector<string>{
          "/bin/sh", "-c", "echo done > $2", kHealthcheckTaskJob
        },
        schedulerID_,
        w.id,
        0  // healtchecks don't use "notifyIfTasksNotRunning"
      );
    } catch (const exception& e) {
      LOG(ERROR) << "Error sending health-check to " << debugString(w)
        << ": " << e.what();  // See DO on the other ERROR in this function.
    }
  });
}

void RemoteWorkerRunner::requestWorkerSuicide(
    const cpp2::BistroWorker& w) noexcept {

  eventBase_->runInEventBaseThread([this, w]() noexcept {
    try {
      shared_ptr<cpp2::BistroWorkerAsyncClient> client{getWorkerClient(w)};
      client->requestSuicide(
        unique_ptr<RequestCallback>(new FunctionReplyCallback(
          [client, w](ClientReceiveState&& state) {
            try {
              client->recv_requestSuicide(state);
            } catch (const exception& e) {
              // DO: Try to differentiate the bad exceptions from the good.
              // I.e. if the worker is already dead, whine less.
              LOG(WARNING) << "Requesting suicide for " << debugString(w)
                << " returned an error. We expect the client to disconnect "
                << " on success, so use your judgement: " << e.what();
            }
          }
        )),
        schedulerID_,
        w.id
      );
    } catch (const exception& e) {
      LOG(ERROR) << "Error sending suicide request to " << debugString(w)
        << ": " << e.what();
    }
  });
}

///
/// Apply actions (side effects) requested by RemoteWorker instances
///
/// NOTE: Runs both in the worker check thread and in the Thrift thread.
///

// DO: Audit for exceptions, and make this function noexcept.
void RemoteWorkerRunner::applyUpdate(RemoteWorkerUpdate* update) {
  // Suicide first, in case a worker also has a healthcheck.
  if (size_t num = update->suicideWorkers().size()) {
    folly::AutoTimer<> timer("Requested suicide from ", num, " workers");
    for (const auto& shard_and_worker : update->suicideWorkers()) {
      requestWorkerSuicide(shard_and_worker.second);
      // We assume that RemoteWorker also issued 'lost tasks' for all of the
      // tasks on this worker.
    }
  }

  // Send out healthchecks
  if (size_t num = update->workersToHealthcheck().size()) {
    folly::AutoTimer<> timer("Sent healthchecks to ", num, " workers");
    auto new_workers = update->newWorkers();
    for (const auto& shard_and_worker : update->workersToHealthcheck()) {
      sendWorkerHealthcheck(
        shard_and_worker.second,
        new_workers.find(shard_and_worker.first) != new_workers.end()
      );
    }
  }

  // Send out checks for 'unsure if running' tasks; the results will be
  // received via the updateStatus Thrift call.
  if (size_t num = update->unsureIfRunningTasksToCheck().size()) {
    for (const auto& p : update->unsureIfRunningTasksToCheck()) {
      checkUnsureIfRunningTasks(p.second.first, p.second.second);
    }
  }

  // Process lost tasks before processing new ones, in case some are replaced.
  if (size_t num = update->lostRunningTasks().size()) {
    folly::AutoTimer<> timer("Updated statuses for ", num, " lost tasks");
    for (const auto& id_and_task : update->lostRunningTasks()) {
      // Note: This **will** decrease the retry count. This makes more sense
      // than neverStarted() since the worker might have crashed **because**
      // of this task.
      auto status = TaskStatus::errorBackoff("Remote worker lost (crashed?)");
      // Ensure that application-specific statuses (if they arrive while
      // the worker is MUST_DIE but still associated) can overwrite the
      // 'lost' status, but the 'lost' status cannot overwrite a real
      // status that arrived between loseRunningTasks and this call.
      status.markOverwriteable();
      // IMPORTANT: Do not call recordFailedTask here because the lost tasks
      // are automatically recorded by RemoteWorker::loseRunningTasks.  See
      // its code for an explanation of how we maintain status consistency
      // between RemoteWorker & TaskStatuses, even though the two updates
      // are not atomic.
      taskStatuses_->updateStatus(id_and_task.second, std::move(status));
    }
  }
  // This may change the state of a worker from NEW to UNHEALTHY. For
  // RemoteWorker::loseRunningTasks, it is important that this happen
  // *after* processing lost tasks.
  fetchRunningTasksForNewWorkers(update);
}

// Ask a worker to notify the scheduler in the event that any of the tasks
// it's unsure about are actually not running.  Since this is an applyUpdate
// helper, it is called concurrently from multiple threads.
void RemoteWorkerRunner::checkUnsureIfRunningTasks(
    const cpp2::BistroWorker& w,
    const std::vector<cpp2::RunningTask>& tasks) {

  eventBase_->runInEventBaseThread([this, tasks, w]() noexcept {
    try {
      auto timer = std::make_shared<folly::AutoTimer<>>();
      shared_ptr<cpp2::BistroWorkerAsyncClient> client{getWorkerClient(w)};
      // The sequence number increment is not atomic with respect to the
      // remote call, and that's okay.  If a runTask queries the sequence
      // number after this increment operation, it is *definitely* not part
      // of our argument "tasks", so we don't care about the order of
      // arrival at the worker of this call vs the runTask.
      int64_t sequence_num = workers_->mutableWorkerOrAbort(w.shard)
        ->calledNotifyIfTasksNotRunning();
      client->notifyIfTasksNotRunning(
        unique_ptr<RequestCallback>(new FunctionReplyCallback(
          // It's a little lame to copy tasks twice, but that's okay.
          [this, tasks, w, client, timer](ClientReceiveState&& state) {
            // TODO(#5025478): Convert this, and the other recv_* calls in
            // this file to use recv_wrapped_*.
            try {
              client->recv_notifyIfTasksNotRunning(state);
            } catch (const std::exception& e) {
              LOG(ERROR) << "notifyIfTasksNotRunning failed: " << e.what();
              return;
            }
            // This is not "clear" because new ones might have been added.
            workers_->mutableWorkerOrAbort(w.shard)
              ->eraseUnsureIfRunningTasks(tasks);
            timer->log(
              "Queried ", tasks.size(), " 'unsure if running ",
              "tasks from worker ", debugString(w), tasks.empty() ? "" :
                (", sample task: " + debugString(tasks.front()))
            );
          }
        )),
        tasks,
        schedulerID_,
        w.id,
        sequence_num
      );
    } catch (const exception& e) {
      LOG(ERROR) << "Error connecting to the worker: " << e.what();
    }
  });
}

// Fetch and record running tasks for the workers that just connected to
// this scheduler.  Since this is an applyUpdate helper, it is called
// concurrently from multiple threads.
void RemoteWorkerRunner::fetchRunningTasksForNewWorkers(
    RemoteWorkerUpdate* update) {

  for (const auto& pair : update->newWorkers()) {
    const auto& w = pair.second;
    eventBase_->runInEventBaseThread([this, w]() noexcept {
      try {
        auto timer = std::make_shared<folly::AutoTimer<>>();
        shared_ptr<cpp2::BistroWorkerAsyncClient> client{getWorkerClient(w)};
        client->getRunningTasks(
          unique_ptr<RequestCallback>(new FunctionReplyCallback(
            [this, w, client, timer](ClientReceiveState&& state) {
              std::vector<cpp2::RunningTask> running_tasks;
              // TODO(#5025478): Convert this, and the other recv_* calls in
              // this file to use recv_wrapped_*.
              try {
                client->recv_getRunningTasks(running_tasks, state);
              } catch (const std::exception& e) {
                LOG(ERROR) << "getRunningTasks failed: " << e.what();
                return;  // RemoteWorker::updateState will trigger a retry
              }
              timer->log(
                "Got ", running_tasks.size(), " running tasks from worker ",
                debugString(w), running_tasks.empty() ? "" :
                  (", sample task: " + debugString(running_tasks.front()))
              );
              // Atomically update RemoteWorker & TaskStatuses
              SYNCHRONIZED(workers_) {
                auto worker = workers_.mutableWorkerOrAbort(w.shard);
                // applyUpdate in another thread could have won (#5176536)
                if (worker->getState() != RemoteWorkerState::State::NEW) {
                  LOG(WARNING) << "Ignoring running tasks for non-new "
                    << w.shard;
                  return;
                }
                worker->initializeRunningTasks(running_tasks);
                // IMPORTANT: Update TaskStatuses **inside** the workers_ lock
                for (const auto& rt : running_tasks) {
                  taskStatuses_->updateStatus(rt, TaskStatus::running());
                }
              }
              timer->log(
                "Recorded ", running_tasks.size() , " new running tasks for ",
                w.shard
              );
            }
          )),
          w.id
        );
      } catch (const exception& e) {
        LOG(ERROR) << "Error connecting to the worker: " << e.what();
      }
    });
  }
}

void RemoteWorkerRunner::killTask(
    const std::string& job,
    const std::string& node,
    cpp2::KilledTaskStatusFilter status_filter) {

  // Look up the running task
  const Job::ID job_id(Job::JobNameTable.asConst()->lookup(job));
  const Node::ID node_id(Node::NodeNameTable.asConst()->lookup(node));
  auto maybe_rt = taskStatuses_->copyRunningTask(job_id, node_id);
  if (!maybe_rt.hasValue()) {
    throw BistroException("Unknown running task ", job, ", ", node);
  }

  // Look up the worker for the task
  folly::Optional<cpp2::BistroWorker> maybe_worker;
  SYNCHRONIZED(workers_) {
    auto* worker_ptr = workers_.getWorker(maybe_rt->workerShard);
    if (worker_ptr != nullptr) {
      maybe_worker = worker_ptr->getBistroWorker();
    }
  }
  if (!maybe_worker.hasValue()) {
    throw BistroException("Could not get worker for ", debugString(*maybe_rt));
  }

  // Make a synchronous kill request so that the client knows when the kill
  // succeeds or fails.  This can take 10 seconds or more.
  apache::thrift::async::TEventBase evb;
  getAsyncClientForAddress<cpp2::BistroWorkerAsyncClient>(
    &evb,
    maybe_worker->addr,
    0,  // default connect timeout
    0,  // default send timeout
    300000  // 5 minute receive timeout
  )->sync_killTask(*maybe_rt, status_filter, schedulerID_, maybe_worker->id);
}

}}
