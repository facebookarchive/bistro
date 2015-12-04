/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/Optional.h>
#include <string>
#include <unordered_map>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

class RemoteWorker;
class RemoteWorkerUpdate;
class TaskStatus;

/**
 * Forwards RemoteWorkerRunner requests to the appropriate
 * RemoteWorker(s).  Provides round-robin selection of workers,
 * either global, or by host.
 *
 * WARNING: Not thread-safe, the caller must provide its own mutex.
 *
 * TODO(reviewers): Right now, RemoteWorkerRunner locks RemoteWorkers.  It's
 * possible that some applications would benefit from locking each
 * individual RemoteWorker.  How can I measure contention on the
 * RemoteWorkers lock and make an intelligent decision about this?
 */
class RemoteWorkers {
private:
//XXX eliminate clowntown
  // Inheritance saves a lot of boilerplate over composition
  class RoundRobinWorkerPool : public std::unordered_map<
    // Map of shard => worker connection
    std::string, std::shared_ptr<RemoteWorker>
  > {
  public:
    explicit RoundRobinWorkerPool(const std::string name) : name_(name) {}

    /**
     * Robust iterator: if nextShard_ isn't in the pool, use a random element.
     * If the pool is empty, returns nullptr.
     */
    const RemoteWorker* getNextWorker();

  private:
    const std::string name_;  // for log messages
    std::string nextShard_;
  };

public:
  RemoteWorkers(
    time_t start_time,
    cpp2::BistroInstanceID scheduler_id
  ) : startTime_(start_time),
      schedulerID_(std::move(scheduler_id)),
      workerPool_("all workers") {
    nonMustDieWorkerSetID_.schedulerID = schedulerID_;
  }

  RemoteWorkers(const RemoteWorkers&) = delete;
  RemoteWorkers& operator=(const RemoteWorkers&) = delete;
  RemoteWorkers(RemoteWorkers&&) = delete;
  RemoteWorkers& operator=(RemoteWorkers&&) = delete;

  folly::Optional<cpp2::SchedulerHeartbeatResponse> processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& worker,
    const cpp2::WorkerSetID& worker_set_id
  );

  void updateState(RemoteWorkerUpdate* update);

  void initializeRunningTasks(
    const cpp2::BistroWorker& worker,
    const std::vector<cpp2::RunningTask>& running_tasks
  );

  RemoteWorker* mutableWorkerOrAbort(const std::string& shard) {
    auto w = getNonConstWorker(shard);
    CHECK(w != nullptr) << "Unknown RemoteWorker: " << shard;
    return w;
  }

  RemoteWorker* mutableWorkerOrThrow(const std::string& shard) {
    auto w = getNonConstWorker(shard);
    if (w == nullptr) {
      throw BistroException("Unknown RemoteWorker: ", shard);
    }
    return w;
  }

  // Return nullptr if there's no worker with this shard ID
  const RemoteWorker* getWorker(const std::string& shard) {
    return getNonConstWorker(shard);
  }

  // Returns nullptr if no worker is available
  const RemoteWorker* getNextWorker() {
    return workerPool_.getNextWorker();
  }

  // Returns nullptr if no worker is available on that host
  const RemoteWorker* getNextWorkerByHost(
    const std::string &hostname
  ) { return mutableHostWorkerPool(hostname).getNextWorker(); }

  // The worker pool accessors deliberately cannot use 'getNextWorker', they
  // are meant only for iterating over the entire pool.
  const RoundRobinWorkerPool& workerPool() const { return workerPool_; }
  const RoundRobinWorkerPool& hostWorkerPool(const std::string& hostname) {
    return mutableHostWorkerPool(hostname);
  }

  // For unit tests
  cpp2::WorkerSetID nonMustDieWorkerSetID() const {
    return nonMustDieWorkerSetID_;
  }
  const std::multiset<cpp2::WorkerSetID>& initialWorkerSetIDs() const {
    return initialWorkerSetIDs_;
  }

private:
  RemoteWorker* getNonConstWorker(const std::string& shard) {
    auto it = workerPool_.find(shard);
    if (it == workerPool_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  /**
   * At startup, the scheduler has to wait for workers to connect, and to
   * report their running tasks, so that we do not accidentally re-start
   * tasks that are already running elsewhere.
   *
   * This call can tell the scheduler to exit initial wait, in one of
   * two circumstances:
   *  - The initial wait expired, meaning that any non-connected workers
   *    would've committed suicide -- thus, we cannot start duplicate tasks.
   *  - Our set of non-MUST_DIE workers matches the initial worker set
   *    returned by every worker: i.e. all extant workers agree that the
   *    connected workers are *all* the workers.  This can shorten the
   *    initial wait dramatically.  See README.worker_set_consensus, as well
   *    as the comments for WorkerSet-related member variables.
   */
  void updateInitialWait(RemoteWorkerUpdate* update);

  /**
   * If hostname isn't found, makes an empty worker pool for more concise code.
   */
  RoundRobinWorkerPool& mutableHostWorkerPool(const std::string& hostname);

  bool inInitialWait_{true};
  time_t startTime_;  // For the "initial wait" computation


  //
  // IMPORTANT: All of the WorkerSetID members must be declared before the
  // worker pools, so that all RemoteWorker cobs get destroyed first.
  //

  // Collects initial WorkerSetIDs of all non-MUST_DIE workers tracked by
  // this scheduler.  These arrive in the first heartbeat from a worker
  // instance, usually the WorkerSetID inherited from the old scheduler.
  //
  // This lets the scheduler exit initial wait quickly at startup -- if all
  // healthy workers have the same initial WorkerSet, and that set matches
  // the current worker set in nonMustDieWorkerSetID_, we conclude that all
  // the workers have connected, and start running tasks.
  //
  // The goal is to allow the use of long --lose_unhealthy_worker_after
  // values, without incurring the operational pain of a long initial wait.
  // Workers can then survive long network partitions and scheduler outages,
  //
  // There is a race between a new worker connecting, and maybe not managing
  // to become part of the consensus, just before a scheduler restart.  The
  // common failure mode is that the new scheduler's workers do *not* agree
  // on a consensus, and the full initial wait is required.  This is safe,
  // if mildly inconvenient.  The less common failure mode is:
  //  (i) A new worker W connects,
  //  (ii) It starts running tasks before joining the consensus,
  //  (iii) The scheduler restarts,
  //  (iv) Other workers connect first, creating a consensus excluding W.
  //  (v) The scheduler starts duplicates of W's tasks.
  // To mitigate this, we implement consensusPermitsBecomingHealthy logic,
  // which prevents starting tasks on a worker until it becomes a provably
  // inextricable part of the consensus.  Surprisingly, this is efficient.
  //
  // Updated via the new/dead worker callbacks.
  std::multiset<cpp2::WorkerSetID> initialWorkerSetIDs_;

  // Collects the instance IDs of all non-MUST_DIE workers in workerPool_.
  // If this matches initialWorkerSetIDs_, we can exit initial wait early.
  // A new worker `w` cannot become eligible to run tasks until it requires
  // all of these workers for consensus, **and** all these workers
  // (indirectly) require `w` for consensus.  README.worker_set_consensus
  // explains why only MUST_DIE is excluded, and other details.
  cpp2::WorkerSetID nonMustDieWorkerSetID_;


  // Only used so that RemoteWorkers can decide whether a WorkerSetID they
  // receive comes from the current scheduler.
  cpp2::BistroInstanceID schedulerID_;


  RoundRobinWorkerPool workerPool_;
  // Per-host round-robin, with the pointers shared with workerPool_
  std::unordered_map<std::string, RoundRobinWorkerPool> hostToWorkerPool_;
};

}}
