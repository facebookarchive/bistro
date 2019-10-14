/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Optional.h>
#include <string>
#include <unordered_map>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/remote/WorkerSetID.h"
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

  using VersionAndShard =
    std::pair<int64_t /*WorkerSetID ver*/, std::string /*shard*/>;
  // Provide a correct "less than" ordering in VersionShardSet even when the
  // version overflows.
  struct VersionAndShardEarlierThan {
    bool operator()(const VersionAndShard& a, const VersionAndShard& b) {
      return WorkerSetIDEarlierThan()(a.first, b.first) || (
        (a.first == b.first) && (a.second < b.second)
      );
    }
  };

public:
  struct HistoryStep {  // Exposed for unit tests
    // Did a worker become MUST_DIE or get replaced at this step?  This
    // doesn't have to be plural because we only prune the *prefix* of
    // history, and thus never accumulate more than one removal per step.
    // `removed` is always applied before `added`, which means that if a
    // shard is added and removed in the same step, it appears in neither.
    folly::Optional<std::string> removed;
    // Newly connected workers added at or before this version (if initial).
    std::unordered_set<std::string> added;
    bool operator==(const HistoryStep& hs) const {
      return hs.removed == removed && hs.added == added;
    }
  };
  // The iterator must survive invalidation, so use std::map.
  using History = std::map<int64_t, HistoryStep, WorkerSetIDEarlierThan>;
  using VersionShardSet =  // Exposed for unit tests
    std::set<VersionAndShard, VersionAndShardEarlierThan>;

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
  const RemoteWorker* getWorker(const std::string& shard) const {
    auto it = workerPool_.find(shard);
    return it == workerPool_.end() ? nullptr : it->second.get();
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

  void manuallyExitInitialWait() { inInitialWait_ = false; }

  // All of these are for unit tests ONLY.
  cpp2::WorkerSetID nonMustDieWorkerSetID() const {
    return nonMustDieWorkerSetID_;
  }
  const std::multiset<cpp2::WorkerSetID>& initialWorkerSetIDs() const {
    return initialWorkerSetIDs_;
  }
  const VersionShardSet& indirectVersionsOfNonMustDieWorkers() const {
    return indirectVersionsOfNonMustDieWorkers_;
  }
  const bool consensusPermitsBecomingHealthyForUnitTest(std::string w) const {
    return consensusPermitsBecomingHealthy(*getWorker(w));
  }
  const History& historyForUnitTest() const { return history_; }

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

  /**
   * Goal: before letting a new worker `w` become healthy, we want it to be
   * well-enough integrated into the other workers' consensus, so that on
   * restart, the scheduler cannot reach an initial consensus that does not
   * include `w`.
   */
  bool consensusPermitsBecomingHealthy(const RemoteWorker& w) const;

  /**
   * Versions that precede the earliest referenced version can be
   * eliminated, since versions can never move backwards.
   *
   * It's not safe to prune unused versions beyond the earliest one, since a
   * worker might potentially return a heartbeat containing a currently
   * unused version, which is ahead of its current one.  This can happen
   * because RemoteWorker::workerSetID() is just an echo of the last
   * RemoteWorkers::nonMustDieWorkerSetID_ that was sent to the worker.
   * It's not reasonable to track all versions we had set to the worker and
   * mark them 'referenced', so we prune conservatively instead.
   */
  void pruneUnusedHistoryVersions();

  /**
   * For each worker, find the highest version required by any worker in its
   * indirect set. See implementation and README.worker_set_consensus.
   */
  void propagateIndirectWorkerSets();

  /**
   * Update nonMustDieWorkersByIndirectVersion_ and w->indirectWorkerSetID_,
   * if this update changes this worker's indirect version.  See the
   * algorithm in README.worker_set_consensus.
   */
  void updateIndirectWorkerSetVersion(RemoteWorker*, const cpp2::WorkerSetID&);

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

  // Denormalizes RemoteWorker::inderctWorkerSetID()->version.  For each
  // worker, this is first set when it echoes the first WorkerSetID from
  // this scheduler, and is removed when the worker becomes MUST_DIE or is
  // replaced.  Updated whenever indirectWorkerSetID() is updated.
  //
  // Firstly, lets consensusPermitsBecomingHealthy() efficiently determine
  // if each worker indirectly requires a particular version (or above).
  //
  // Secondly, propagateIndirectWorkerSets() needs an efficient* way to
  // iterate through the workers in increasing order of their
  // indirectWorkerSetID version.
  //
  // * The shard in the key makes updates easier to impelement and more
  // robust (since this becomes a unique-element set). In the unlikely event
  // that hashing the shard ID hurts your perf, there is a way to store a
  // RemoteWorker pointer here, it's just more error-prone.
  VersionShardSet indirectVersionsOfNonMustDieWorkers_;

  // Collects the instance IDs of all non-MUST_DIE workers in workerPool_.
  // history_ is effectively a changelog of this set.
  //
  // Firstly, the scheduler can exit initial wait early if this set's hash
  // matches the consensus set of initialWorkerSetIDs_.
  //
  // Secondly, a new worker `w` cannot become eligible to run tasks until it
  // requires all of these workers for consensus, **and** all these workers
  // (indirectly) require `w` for consensus.
  //
  // README.worker_set_consensus explains why only MUST_DIE is excluded, and
  // other details.
  cpp2::WorkerSetID nonMustDieWorkerSetID_;

  // nonMustDieWorkerSetID_ includes a version that points into the
  // following history structure.  It is an ordered list of versions, each
  // entry describes a change in the worker set.
  //
  // IMPORTANT: In order to safely handle the eventual overflow, always use
  // WorkerSetIDEarlierThan to compare history versions.
  History history_;

  // Only used so that RemoteWorkers can decide whether a WorkerSetID they
  // receive comes from the current scheduler.
  cpp2::BistroInstanceID schedulerID_;


  RoundRobinWorkerPool workerPool_;
  // Per-host round-robin, with the pointers shared with workerPool_
  std::unordered_map<std::string, RoundRobinWorkerPool> hostToWorkerPool_;
};

}}
