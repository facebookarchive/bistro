/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/runners/RemoteWorkerRunner.h"

#include <cmath>

#include <folly/experimental/AutoTimer.h>
#include <folly/gen/Base.h>

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

namespace facebook { namespace bistro {

using namespace std;
using namespace apache::thrift::async;
using namespace apache::thrift;

RemoteWorkerRunner::RemoteWorkerRunner(
    std::shared_ptr<TaskStatuses> task_statuses,
    std::shared_ptr<Monitor> monitor,
    WorkerClientFn workerClientFn)
    : TaskRunner(), // Initializes schedulerID_
      workerClientFn_(workerClientFn),
      workers_(folly::in_place, time(nullptr), schedulerID_),
      workerLevel_(StringTable::NotFound),
      taskStatuses_(std::move(task_statuses)),
      eventBase_(new folly::EventBase()),
      eventBaseThread_(bind(&folly::EventBase::loopForever, eventBase_.get())),
      inInitialWait_(true),
      monitor_(std::move(monitor)) {
  DCHECK(workerClientFn_) << "workerClientFn is nullptr";
  // Monitor the workers: send healthchecks, mark them (un)healthy / lost, etc
  //
  // CAUTION: ThreadedRepeatingFunctionRunner recommends two-stage
  // initialization for starting threads.  This specific case is safe since:
  //  - this comes last in the constructor, so the class is fully constructed,
  //  - this class is final, so no derived classes remain to be constructed.
  backgroundThreads_.add("RemoteWrkrRnnr", [this]() {
    auto config = config_.copy();
    bool log_manually_exit_initial_wait = false;
    RemoteWorkerUpdate update;
    SYNCHRONIZED(workers_) {
      workers_.updateState(&update);
      if (!update.initialWaitMessage().empty()
          && config  // Can be nullptr before the first updateConfig
          && config->exitInitialWaitBeforeTimestamp > update.curTime()) {
        workers_.manuallyExitInitialWait();
        update.setInitialWaitMessage(std::string());
        // Log below to minimize the workers_ lock time.
        log_manually_exit_initial_wait = true;
      }
    }

    // This cannot be in applyUpdate, since initial wait is only computed in
    // updateState() and not in processHeartbeat().
    //
    // It is safe to end the initial wait before doing anything else, since we
    // update TaskStatuses immediately after initializeRunningTasks().  I.e.,
    // if we think that all workers have connected, then the scheduler is also
    // aware of all their running tasks by this point.
    DEFINE_MONITOR_ERROR(monitor_, error, "RemoteWorkerRunner initial wait");
    if (update.initialWaitMessage().empty()) {
      if (log_manually_exit_initial_wait) {
        LOG(WARNING) << "Exiting initial wait due to a MANUAL override via "
          << "the config key '" << kExitInitialWaitBeforeTimestamp << "', "
          << "which is set to " << config->exitInitialWaitBeforeTimestamp
          << ", while the current time is " << update.curTime()
          << ". DANGER: Do not overuse this, since accidentally exiting "
          << "initial wait when there are tasks running on live workers "
          << "***WILL*** cause you to double-start tasks.";
      }
      inInitialWait_.store(false, std::memory_order_relaxed);
      // No call to 'error' clears the "intial wait" errors from the UI.
    } else {
      LOG(WARNING) << error.report(update.initialWaitMessage());
    }

    applyUpdate(&update);
    return chrono::seconds(RemoteWorkerState::workerCheckInterval());
  });
}

RemoteWorkerRunner::~RemoteWorkerRunner() {
  backgroundThreads_.stop();
  eventBase_->terminateLoopSoon();
  eventBaseThread_.join();
}

namespace {
// Clamp to the range of the physical output.
template <typename T>
void clampPhysical(T* phys, double unclamped, const cpp2::Resource& r) {
  if (unclamped < 0) {  // Resources are unsigned, even if Thrift won't let us.
    *phys  = 0;
  } else if (unclamped > std::numeric_limits<T>::max()) {
    *phys = std::numeric_limits<T>::max();
  } else {
    *phys = unclamped;
    return;
  }
  LOG(WARNING) << "Clamping physical value of " << r.name << " from "
    << unclamped << " to limit " << *phys;
}

template <typename T>
void logicalToIntPhysicalResource(
    T* phys,
    const cpp2::PhysicalResourceConfig& p,
    const cpp2::Resource& r) {
  clampPhysical(phys, ::round(p.multiplyLogicalBy * r.amount), r);
}

folly::Optional<int> physicalToLogicalResource(
    const cpp2::PhysicalResourceConfig& c,
    double physical) {
  // At present, physical resources set to 0 are "not available", so let
  // them go as defaults.
  if (physical == 0) {
    return folly::none;
  }
  auto r = ::trunc((physical - c.physicalReserveAmount) / c.multiplyLogicalBy);
  return r < 0 ? 0 : r;
}
}  // anonymous namespace

/* static */
RemoteWorkerRunner::WorkerClientFn
RemoteWorkerRunner::defaultWorkerClientFunction() {
  return [](folly::EventBase* eventBase, const cpp2::ServiceAddress& addr) {
    return getAsyncClientForAddress<cpp2::BistroWorkerAsyncClient>(
      eventBase, addr);
  };
}

void RemoteWorkerRunner::updateConfig(std::shared_ptr<const Config> config) {
  folly::AutoTimer<> timer;
  DEFINE_MONITOR_ERROR(monitor_, error, "RemoteWorkerRunner resource update");

  // Memoize these two values to be used by the next runTask
  workerLevel_ = config->levels.lookup("worker");
  CHECK(workerLevel_ != config->levels.NotFound);
  SYNCHRONIZED(config_) {
    config_ = config;
  }

  const auto& def_resources = config->resourcesByLevel[workerLevel_];
  // Always lock workerResources_ first, then workers_
  SYNCHRONIZED(workerResources_) { SYNCHRONIZED_CONST(workers_) {
    workerResources_.clear();
    // While it __looks__ like we no longer need 'workers' to be locked
    // after this loop, that lock is actually simultaneously ensuring that
    // RemoteWorkerRunner::runTaskImpl() gets to mark the task running in
    // the scheduler *BEFORE* the next updateWorkerResources().  So, there
    // would be no savings by releasing the lock before we are done here.
    for (const auto& wconn : workers_.workerPool()) {
      const auto& w = wconn.second->getBistroWorker();

      auto& w_res = workerResources_[w.shard];
      w_res = def_resources;  // Start with the defaults.

      // Apply any known physical resources.
      for (const auto& prc : config->physicalResourceConfigs) {
        // Apply CPU, RAM, # of GPU cards.
        if (const auto val = [&]() -> folly::Optional<int> {
          switch (prc.physical) {
            case cpp2::PhysicalResource::RAM_MBYTES:
              return
                physicalToLogicalResource(prc, w.usableResources.memoryMB);
            case cpp2::PhysicalResource::CPU_CORES:
              return
                physicalToLogicalResource(prc, w.usableResources.cpuCores);
            case cpp2::PhysicalResource::GPU_CARDS:
              return
                physicalToLogicalResource(prc, w.usableResources.gpus.size());
            default:
              return folly::none;
          }
        }()) {
          CHECK_GE(prc.logicalResourceID, 0);
          CHECK_LT(prc.logicalResourceID, w_res.size());
          w_res[prc.logicalResourceID] = *val;
        }
        // If the user configured special resources for GPU card models,
        // populate them too.
        if (prc.physical == cpp2::PhysicalResource::GPU_CARDS) {
          for (const auto& gpu : w.usableResources.gpus) {
            auto r_name = "GPU: " + gpu.name;
            auto rid = config->resourceNames.lookup(r_name);
            if (rid != StringTable::NotFound) {
              CHECK_GE(rid, 0);
              if (rid >= w_res.size()
                  || w_res[rid] == numeric_limits<int>::max()) {
                // Ok to logspam, since it's probably a misconfiguration.
                LOG(WARNING) << "Resource " << r_name << " exists, but is "
                  << "not a worker resource. Ignoring.";
              } else {
                // Let's hope these resources have default limits of 0 :)
                ++w_res[rid];
              }
            }
          }
        }
      }

      // Apply manual worker resource overrides.
      // Try for hostport, and fallback to hostname, then shard name
      auto it = config->workerResourcesOverride.find(
        folly::to<string>(w.machineLock.hostname, ':', w.machineLock.port)
      );
      if (it == config->workerResourcesOverride.end()) {
        it = config->workerResourcesOverride.find(w.machineLock.hostname);
      }
      if (it == config->workerResourcesOverride.end()) {
        // I added the shard-name lookup for TestBusiestSelector, but it
        // seems like a reasonable idea in general.
        it = config->workerResourcesOverride.find(w.shard);
      }

      if (it != config->workerResourcesOverride.end()) {
        for (const auto& p : it->second) {
          w_res[p.first] = p.second;
        }
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

namespace {
/**
 * This helper is separate from RemoteWorkerRunner::getJobLogs to
 * make it clear that it does not use any member variables (i.e.
 * it's effectively a pure function).
 *
 * Importantly, it must not access the current thread's EventBase, since
 * this is typically called from an EventBase handler, and event_base_loop()
 * is not reentrant.
 */
LogLines getJobLogsThreadAndEventBaseSafe(
    const std::string& unqueried_workers,
    const std::vector<cpp2::ServiceAddress>& services,
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    const string& regex_filter,
    RemoteWorkerRunner::WorkerClientFn workerClientFn) {

  LogLines res;
  res.nextLineID = LogLine::kNotALineID;
  time_t cur_time = time(nullptr);  // timestamp for any errors

  if (!unqueried_workers.empty()) {
    auto err = folly::to<string>(
      "Warning: some workers are unhealthy, so we cannot return logs from "
      "them. If your task ever ran on any of the following workers, these "
      "results may be incomplete -- ", unqueried_workers
    );
    LOG(WARNING) << err;
    res.lines.emplace_back("", "", cur_time, err, LogLine::kNotALineID);
  }

  // Manages a bunch of concurrent requests to get the logs from the workers.
  // DANGER: Do not replace by getEventBase(), see the docstring.
  folly::EventBase eb;

  auto resultsx = folly::collectAll(
      folly::gen::from(services)
      | folly::gen::mapped([&](auto const& addr) {
        return folly::makeFutureWith([&]{
          auto client = workerClientFn(&eb, addr);
          return client->future_getJobLogsByID(
              logtype,
              jobs,
              nodes,
              line_id,
              is_ascending,
              // at least 100 lines per worker, but try to stay under 5000
              // lines total
              max(5000 / int(services.size()), 100),
              regex_filter);
        })
        .thenError([&](folly::exception_wrapper&& ew) {
          auto const service_id = debugString(addr);
          // Logging is done here so that errors are emitted as they happen,
          // rather than all at once at the end.
          auto const err = folly::to<string>(
            "Failed to fetch logs from worker ", service_id, ", this may be ",
            "transient -- but if it is not, report it: ", ew.what()
          );
          LOG(ERROR) << err;
          res.lines.emplace_back("", "", cur_time, err, LogLine::kNotALineID);
          return folly::makeFuture<cpp2::LogLines>(std::move(ew));
        });
      })
      | folly::gen::as<std::vector>())
    .getVia(&eb);

  auto const results = folly::gen::from(resultsx)
    | folly::gen::filter([](auto const& _) { return _.hasValue(); })
    | folly::gen::mapped([](auto& _) { return _.value(); })
    | folly::gen::move
    | folly::gen::as<std::vector>();

  // Find the most restrictive nextLineID among all hosts
  for (const auto& log : results) {
    if (
      // This value marks "no more lines" in LogWriter, so exclude it.
      (log.nextLineID != LogLine::kNotALineID) && (
        // This line only matches initially due to the previous comparison
        (res.nextLineID == LogLine::kNotALineID) ||
        ((res.nextLineID > log.nextLineID) == is_ascending)
      )
    ) {
      res.nextLineID = log.nextLineID;
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
    for (const cpp2::LogLine& l : log.lines) {
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

  // Sort the merged results from all the workers.
  if (is_ascending) {
    sort(
      res.lines.begin(),
      res.lines.end(),
      [](const LogLine& a, const LogLine& b) { return a.lineID < b.lineID; }
    );
  } else {
    sort(
      res.lines.begin(),
      res.lines.end(),
      [](const LogLine& a, const LogLine& b) { return a.lineID > b.lineID; }
    );
  }

  return res;
}
}  // anonymous namespace

LogLines RemoteWorkerRunner::getJobLogs(
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    const string& regex_filter) const {

  // We are going to query all the workers. This is wasteful, but it makes
  // it much easier to find logs for tasks, because:
  //  1) Multiple workers can have logs for different iterations of a task
  //  2) The logs API supports multi-queries, which, in some cases,
  //     require us to query all workers anyhow.
  std::vector<cpp2::ServiceAddress> services;
  std::vector<std::string> unhealthy_workers;
  std::vector<std::string> lost_workers;
  SYNCHRONIZED_CONST(workers_) {
    for (const auto& wconn : workers_.workerPool()) {
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

  return getJobLogsThreadAndEventBaseSafe(
    unqueried_workers,
    services,
    logtype,
    jobs,
    nodes,
    line_id,
    is_ascending,
    regex_filter,
    workerClientFn_
  );
}

cpp2::SchedulerHeartbeatResponse RemoteWorkerRunner::processWorkerHeartbeat(
    const cpp2::BistroWorker& worker,
    const cpp2::WorkerSetID& worker_set_id,
    RemoteWorkerUpdate update,
    std::function<void()> unit_test_cob) {

  // Throws on protocol version mismatch, does not add worker to pool.
  auto r = workers_->processHeartbeat(&update, worker, worker_set_id);
  // In TaskExitedRacesTaskLost, we need to inject logic between making the
  // update and applying it.
  unit_test_cob();
  // This will often result in the healthcheck or getRunningTasks arriving
  // before the heartbeat response does (meaning the worker does not yet
  // know the scheduler).  As a result, "new worker" healthchecks are
  // special-cased in BistroWorkerHandler::runTask().
  applyUpdate(&update);
  if (!r.has_value()) {
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
  const Node& node,
  cpp2::RunningTask& rt,
  folly::dynamic& job_args,
  function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  if (inInitialWait_.load(std::memory_order_relaxed)) {
    return DoNotRunMoreTasks;
  }
  auto config = config_.copy();
  CHECK(config) << "Cannot runTask before updateConfig";

  // Make a copy so we don't have to lock workers_ while waiting for Thrift.
  cpp2::BistroWorker worker;
  // unused initialization to hush a Clang warning:
  int64_t did_not_run_sequence_num = 0;
  // These will modify the task's cgroupOptions if we find a worker.
  int16_t cgroup_cpu_shares = 0;
  int64_t cgroup_memory_limit_in_bytes = 0;
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
        config->remoteWorkerSelectorType
      )->findWorker(
        config.get(),
        *job,
        node,
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
      // Future: support this in LocalRunner mode as well.
      auto& resources_by_node = job_args["resources_by_node"];
      CHECK(resources_by_node.find(rt.workerShard)
            == resources_by_node.items().end())
        << "Cannot have both a node and a worker named: " << rt.workerShard
        << " -- if you are running a worker and the central scheduler on the "
        << "same host, you should specify --instance_node_name global.";
      if (const auto* nr = addNodeResourcesToRunningTask(
        &rt,
        &resources_by_node,
        // This config is stale, but it's consistent with workerResources_.
        *config,
        rt.workerShard,
        workerLevel_,
        job->resources()
      )) {
        for (const auto& r : nr->resources) {
          auto phys_it = config->logicalToPhysical.find(r.name);
          if (phys_it != config->logicalToPhysical.end()) {
            const auto& p =  // at() is like CHECK, since this is `noexcept`
              config->physicalResourceConfigs.at(phys_it->second);
            if (
              p.physical == cpp2::PhysicalResource::RAM_MBYTES
              && p.enforcement == cpp2::PhysicalResourceEnforcement::HARD
            ) {
              logicalToIntPhysicalResource(
                &cgroup_memory_limit_in_bytes, p, r
              );
            } else if (
              p.physical == cpp2::PhysicalResource::CPU_CORES
              && p.enforcement == cpp2::PhysicalResourceEnforcement::SOFT
            ) {
              logicalToIntPhysicalResource(&cgroup_cpu_shares, p, r);
              // Multiply by 2 here since cgroup cpu.shares must be >= 2.
              clampPhysical(&cgroup_cpu_shares, 2*cgroup_cpu_shares, r);
            }
          }
        }
      }

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
      // IMPORTANT: Update TaskStatuses **inside** the workers_ lock,
      // otherwise e.g.  a worker could get lost before cb runs, failing the
      // TaskStatusSnapshot::updateStatus() check for "when a task is not
      // running, the received status is not overwritable".
      cb(rt, std::move(status));
    }
  }

  // Now we have a healthy worker with resources we already grabbed. It may
  // get marked unhealthy or even lost, since workers_ is now unlocked, but
  // we just have to try our luck.
  eventBase_->runInEventBaseThread([
      cb, this, job, rt, job_args, worker, did_not_run_sequence_num,
      cgroup_cpu_shares, cgroup_memory_limit_in_bytes
    ]() noexcept {
    try {
      shared_ptr<cpp2::BistroWorkerAsyncClient> client =
        workerClientFn_(eventBase_.get(), worker.addr);
      auto task_subproc_opts = job->taskSubprocessOptions();
      task_subproc_opts.cgroupOptions.cpuShares = cgroup_cpu_shares;
      task_subproc_opts.cgroupOptions.memoryLimitInBytes =
        cgroup_memory_limit_in_bytes;
      client->runTask(
        unique_ptr<RequestCallback>(new FunctionReplyCallback(
          [this, cb, client, rt, worker, did_not_run_sequence_num](
              ClientReceiveState&& state) noexcept {
            // TODO(#5025478): Convert this, and the other recv_* calls in
            // this file to use recv_wrapped_*.
            try {
              client->recv_runTask(state);
            } catch (const cpp2::BistroWorkerException& e) {
              LOG(ERROR) << "Worker never started task "
                << debugString(rt) << ": " << e.message;
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
              LOG(ERROR) << "The runTask request hit an error on "
                << debugString(rt) << ", will have to poll to find if the "
                "task is running: " << e.what();
              SYNCHRONIZED(workers_) {
                workers_.mutableWorkerOrAbort(worker.shard)
                  ->addUnsureIfRunningTask(rt);
              }
            }
          }
        )),
        rt,
        folly::toJson(job_args),
        job->command().empty()
          ? std::vector<std::string>{/*--worker_command*/} : job->command(),
        schedulerID_,
        worker.id,
        // The real sequence number may have been incremented after we found
        // the worker, but this is okay, the worker simply rejects the task.
        did_not_run_sequence_num,
        std::move(task_subproc_opts)
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

// We deliberately don't track healthchecks that we send. This is a tiny bit
// faster, and prevents them from being shown in the UI.  To match,
// RemoteWorker::updateTaskStatus does not call updateRunningTasks.
void RemoteWorkerRunner::sendWorkerHealthcheck(
    const cpp2::BistroWorker& w, bool is_new_worker) noexcept {

  eventBase_->runInEventBaseThread([this, w, is_new_worker]() noexcept {
    // Healthchecks distribute the latest cgroup configuration to the
    // workers.  This has some mild side effects, but overall they are good,
    // since health-checks will run in a similar setup to normal user jobs.
    //
    // Future: it might be a good idea to make `healthcheck` a proper
    // job, so that this can be configured specially.
    cpp2::TaskSubprocessOptions task_subprocess_opts;
    SYNCHRONIZED(config_) {
      if (config_) {
        task_subprocess_opts = config_->taskSubprocessOptions;
      }
    }
    try {
      shared_ptr<cpp2::BistroWorkerAsyncClient> client =
        workerClientFn_(eventBase_.get(), w.addr);
      // Make a barebones RunningTask; only job, startTime & shard are be used.
      cpp2::RunningTask rt;
      rt.job = kHealthcheckTaskJob;
      // BistroWorkerHandler::runTask doesn't check the scheduler ID for new
      // worker healthchecks.
      rt.node = is_new_worker ? kHealthcheckTaskNewWorkerNode : "";
      rt.invocationID.startTime = time(nullptr);
      // Leaving .rand == 0 for healthchecks makes their cgroups easy to find.
      rt.workerShard = w.shard;
      // .nextBackoffDuration is not applicable
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
        0,  // healtchecks don't use "notifyIfTasksNotRunning"
        task_subprocess_opts
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
      shared_ptr<cpp2::BistroWorkerAsyncClient> client =
        workerClientFn_(eventBase_.get(), w.addr);
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
    folly::AutoTimer<> timer(
        folly::to<std::string>("Requested suicide from ", num, " workers"));
    for (const auto& shard_and_worker : update->suicideWorkers()) {
      requestWorkerSuicide(shard_and_worker.second);
      // We assume that RemoteWorker also issued 'lost tasks' for all of the
      // tasks on this worker.
    }
  }

  // Send out healthchecks
  if (size_t num = update->workersToHealthcheck().size()) {
    folly::AutoTimer<> timer(
        folly::to<std::string>("Sent healthchecks to ", num, " workers"));
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
    folly::AutoTimer<> timer(
        folly::to<std::string>("Updated statuses for ", num, " lost tasks"));
    for (const auto& id_and_task : update->lostRunningTasks()) {
      const auto& original_rt = id_and_task.second;
      // When the scheduler loses a worker, the worker **should** also time
      // out and try to kill all its task ASAP, even if there is a network
      // partition.  However, its "suicide" handler uses a TERM-wait-KILL
      // policy, leaving a short delay to let the tasks exit gracefully.
      //
      // Therefore, the scheduler cannot safely start a new instance of a
      // lost task until this delay has elapsed.  "Backoff" is the normal
      // way of specifying this in the scheduler.  However, this means the
      // scheduler sets a higher effective backoff than what was set by the
      // task's policy.  Then, `workerLost()` undergoes some contortions to
      // save the policy-configured task backoff, so that the next task
      // instance correctly computes the `getNext()` backoff.
      //
      // We add an extra safety margin because even with a SIGKILL, task
      // death is not instant, and the "task finished" message may take some
      // more time to travel from the worker to the scheduler.
      //
      // This does not deal with scenarios where the scheduler goes down and
      // loses the backoff state.  This also does not handle situations
      // where task termination or worker status delivery is so slow that
      // the timeouts below are grossly violated.  Setting a high enough
      // value of --CAUTION_worker_suicide_backoff_safety_margin_sec should
      // help mitigate this.
      //
      // Future: If TaskSubprocessOpts were a member of RunningTask, it
      // would be best to also add 10 * rt.subprocessOpts.pollMs here
      // (this assumses that the EventBase threads aren't *too* backed up.
      auto tweaked_rt = id_and_task.second;
      const int32_t kSafeBackoffSec =
        RemoteWorkerState::workerSuicideBackoffSafetyMarginSec()
          + (tweaked_rt.workerSuicideTaskKillWaitMs == 0
              ? RemoteWorkerState::workerSuicideTaskKillWaitMs()
              : tweaked_rt.workerSuicideTaskKillWaitMs) / 1000
          + 1;  // a lazy way of rounding up, same as in my "initial wait" math
      if (kSafeBackoffSec > original_rt.nextBackoffDuration.seconds) {
        // This safe value will be used even if `noMoreBackoffs` is true,
        // and the task is forgiven -- `TaskStatus::forgive()` makes a
        // special provision for this.
        tweaked_rt.nextBackoffDuration.seconds = kSafeBackoffSec;
      }
      // IMPORTANT: Do not call recordFailedTask here because the lost tasks
      // are automatically recorded by RemoteWorker::loseRunningTasks.  See
      // its code for an explanation of how we maintain status consistency
      // between RemoteWorker & TaskStatuses, even though the two updates
      // are not atomic.
      taskStatuses_->updateStatus(tweaked_rt, TaskStatus::workerLost(
        original_rt.workerShard,
        // Save the unmodified nextBackoffDuration value, since this is the
        // job-configured backoff value the status would have had **after**
        // the update().  Storing it before the update() is hacky, but who's
        // watching?
        original_rt.nextBackoffDuration.seconds
      ));
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
      shared_ptr<cpp2::BistroWorkerAsyncClient> client =
        workerClientFn_(eventBase_.get(), w.addr);
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
        shared_ptr<cpp2::BistroWorkerAsyncClient> client =
          workerClientFn_(eventBase_.get(), w.addr);
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
                workers_.initializeRunningTasks(w, running_tasks);
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
    const cpp2::RunningTask& rt,
    const cpp2::KillRequest& req) {

  // Look up the worker for the task
  folly::Optional<cpp2::BistroWorker> maybe_worker;
  SYNCHRONIZED(workers_) {
    auto* worker_ptr = workers_.getWorker(rt.workerShard);
    if (worker_ptr != nullptr) {
      maybe_worker = worker_ptr->getBistroWorker();
    }
  }
  if (!maybe_worker.has_value()) {
    throw BistroException("Could not get worker for ", debugString(rt));
  }

  // Make a synchronous kill request so that the client knows when the kill
  // succeeds or fails.  This can take 10 seconds or more.
  folly::EventBase evb;
  workerClientFn_(&evb, maybe_worker->addr)
    ->sync_killTask(rt, schedulerID_, maybe_worker->id, req);
}

}}
