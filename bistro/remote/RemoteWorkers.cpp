/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/remote/RemoteWorkers.h"

#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/utils/Exception.h"

DEFINE_int32(
  CAUTION_startup_wait_for_workers, -1,
  "At startup, the scheduler has to wait for workers to connect, so that "
  "we do not accidentally re-start tasks that are already running elsewhere. "
  "The default of -1 computes a 'minimum safe wait' from your healthcheck "
  "and worker loss timeouts. DANGER 1: If you reduce these timeouts from one "
  "scheduler run to the next, the new default ('safe') wait may not be long "
  "enough, so either increase this when reducing timeouts, or take down "
  "all your workers. DANGER 2: Do not use an 'unsafe' value. Imagine a "
  "network partition (e.g. your discovery service is down), which makes some "
  "workers unable to connect longer than your unsafe initial wait.  Then, "
  "it's a virual certainty that the scheduler will at some point be (a) out "
  "of initial wait, (b) will not have all of the workers that have running "
  "tasks -- and therefore it will double-start some already-running tasks. "
  "NOTE 1: As a precaution, the scheduler will not exit initial wait while "
  "it has 'new' workers that have not yet replied with their running tasks. "
  "NOTE 2: Once the initial wait time passes, the scheduler becomes "
  "eligible to exit initial wait, even if the healthy workers's worker set "
  "consensus does not perfectly match our non-MUST_DIE worker set -- at the "
  "time of writing, I see no benefit to waiting longer. For more detail, "
  "see README.worker_set_consensus."
);

namespace facebook { namespace bistro {

using namespace std;
using apache::thrift::debugString;

folly::Optional<cpp2::SchedulerHeartbeatResponse>
RemoteWorkers::processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& worker) {

  // It's best not to add the bad worker to the pool, so check outside of
  // RemoteWorker.  At present, we do not tell that worker to commit
  // suicide, so version mismatches will cause lots of logspam.
  enforceWorkerSchedulerProtocolVersion(
    worker.protocolVersion, cpp2::common_constants::kProtocolVersion()
  );
  const auto& shard = worker.shard;
  auto worker_it = workerPool_.find(shard);
  // Make a RemoteWorker if the shard is new
  if (worker_it == workerPool_.end()) {
    // Even though we're adding another element, the "nextShard_"
    // round-robin iterators need not be updated, since new elements should
    // be distributed randomly throughout the hash tables.
    auto res = workerPool_.emplace(
      shard,
      std::make_shared<RemoteWorker>(update->curTime(), worker)
    );
    // Add the same pointer to the right host worker pool
    CHECK(mutableHostWorkerPool(worker.machineLock.hostname).emplace(
      shard, res.first->second
    ).second) << "Worker pool for hostname " << worker.machineLock.hostname
      << " already had " << " shard " << shard;
    worker_it = res.first;
  } else {  // A RemoteWorker with this shard ID already exists
    // If the hostname changed, move the worker to the new host pool
    const auto& old_hostname =
      worker_it->second->getBistroWorker().machineLock.hostname;
    const auto& new_hostname = worker.machineLock.hostname;
    if (new_hostname != old_hostname) {
      // This might "invalidate" the nextShard_ iterator, but it's okay
      // since the getNextWorker() implementation is robust.
      CHECK(1 == mutableHostWorkerPool(old_hostname).erase(shard))
        << "Inconsistency: did not find shard " << shard
        << " in the worker pool for its hostname " << old_hostname;
      CHECK(mutableHostWorkerPool(new_hostname).emplace(
        shard, worker_it->second
      ).second)
        << "Changing hostname " << old_hostname << " to " << new_hostname
        << ": target already had shard " << shard;
    }
  }
  // Update the worker's state (also update the hostname if needed)
  auto response = worker_it->second->processHeartbeat(
    update,
    worker,
    true
  );
  // NB: We cannot call updateInitialWait() here since it relies on knowing
  // whether any of the workers are new, and doing that here makes each
  // heartbeat take O(# workers).
  return std::move(response);
}

void RemoteWorkers::updateInitialWait(RemoteWorkerUpdate* update) {
  // Future: Maybe remove everything in update->suicideWorkers_ from the
  // worker pools?

  std::string msg;
  if (!inInitialWait_) {
    update->setInitialWaitMessage(std::move(msg));
    return;
  }

  time_t min_safe_wait =
    RemoteWorkerState::maxHealthcheckGap() +
    RemoteWorkerState::loseUnhealthyWorkerAfter() +
    RemoteWorkerState::workerCheckInterval();  // extra safety gap
  time_t min_start_time = update->curTime();
  if (FLAGS_CAUTION_startup_wait_for_workers < 0) {
    min_start_time -= min_safe_wait;
  } else {
    min_start_time -= FLAGS_CAUTION_startup_wait_for_workers;
    if (RemoteWorkerState::maxHealthcheckGap()
        > FLAGS_CAUTION_startup_wait_for_workers) {
      msg += folly::to<std::string>(
        "DANGER! DANGER! Your --CAUTION_startup_wait_for_workers ",
        "of ", FLAGS_CAUTION_startup_wait_for_workers,
        " is lower than the max healthcheck gap of ",
        RemoteWorkerState::maxHealthcheckGap(), ", which makes it very ",
        "likely that you will start second copies of tasks that are ",
        "already running (unless your heartbeat interval is much smaller). "
      );
    } else if (min_safe_wait > FLAGS_CAUTION_startup_wait_for_workers) {
      msg += folly::to<std::string>(
        "Your custom --CAUTION_startup_wait_for_workers is ",
        "less than the minimum safe value of ", min_safe_wait,
        " -- this increases the risk of starting second copies of tasks ",
        "that were already running. "
      );
    }
  }

  // The scheduler is eligible to exit initial wait if:
  //  (i) there are no NEW workers, AND
  //  (ii) the wait expired
  if (min_start_time < startTime_) {
    msg += "Waiting for all workers to connect before running tasks.";
  // If we are eligible to exit initial wait, but are still querying running
  // tasks, then one of the 'new' workers (while transiently unresponsive)
  // might be running tasks the scheduler does not know about.  To be safe,
  // stay in initial wait until all getRunningTasks succeed.
  //
  // This test is why we cannot call updateInitialWait from processHeartbeat.
  } else if (!update->newWorkers().empty()) {
    msg += folly::to<std::string>(
      "Ready to exit initial wait, but not all workers' running tasks were "
      "fetched; not allowing tasks to start until all are fetched."
    );
  } else {
    inInitialWait_ = false;
    msg = "";
  }
  update->setInitialWaitMessage(std::move(msg));
}

void RemoteWorkers::updateState(RemoteWorkerUpdate* update) {
  // Important to check this, but it's silly to check it inside the loop.
  CHECK(FLAGS_healthcheck_period > 0)
        << "--healthcheck_period must be positive";
  for (auto& pair : workerPool_) {
    pair.second->updateState(update, true);
  }
  // Must come after updateState() since it relies on update->newWorkers()
  updateInitialWait(update);
}

void RemoteWorkers::initializeRunningTasks(
    const cpp2::BistroWorker& w,
    const std::vector<cpp2::RunningTask>& running_tasks) {

  auto worker = mutableWorkerOrAbort(w.shard);
  // applyUpdate in another thread could have won (#5176536)
  if (worker->getState() != RemoteWorkerState::State::NEW) {
    LOG(WARNING) << "Ignoring running tasks for non-new " << w.shard;
    return;
  }
  worker->initializeRunningTasks(running_tasks);
}

const RemoteWorker*
RemoteWorkers::RoundRobinWorkerPool::getNextWorker() {

  if (this->empty()) {
    LOG(WARNING) << "No workers in the '" << name_ << "' pool";
    return nullptr;
  }
  auto ret = this->find(nextShard_);
  if (ret == this->end()) {
    ret = this->begin();
  }
  auto* worker = ret->second.get();
  nextShard_ = (++ret == this->end()) ? this->begin()->first : ret->first;
  return worker;
}

RemoteWorkers::RoundRobinWorkerPool&
    RemoteWorkers::mutableHostWorkerPool(const string& host) {

  auto host_worker_it = hostToWorkerPool_.find(host);
  if (host_worker_it == hostToWorkerPool_.end()) {
    host_worker_it = hostToWorkerPool_.emplace(
      host, RoundRobinWorkerPool(host)  // Construct only if needed
    ).first;
  }
  return host_worker_it->second;
}

}}
