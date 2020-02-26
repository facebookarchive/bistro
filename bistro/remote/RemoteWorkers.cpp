/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/remote/RemoteWorkers.h"

#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/remote/WorkerSetID.h"
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
DEFINE_int32(
  CAUTION_worker_suicide_backoff_safety_margin_sec, 60,
  "When a worker commits suicide, it will TERM-wait-KILLs its tasks. Since "
  "task termination is not instant, the scheduler needs a safety margin to "
  "add into its 'safe initial wait' calculation, and also into the "
  "'minimum backoff for tasks of lost workers'. This margin must also "
  "indirectly account for the fact that Bistro's workers poll running tasks "
  "tasks only once every 'poll_ms' -- so keep this margin large."
);
DEFINE_int32(
  CAUTION_worker_suicide_task_kill_wait_ms, 5000,
  "The scheduler (NOT the worker -- the worker ignores this flag) includes "
  "this value with every task it starts. When, in the future, the worker "
  "commits suicide (i.e. due to a too-long network partition, or a scheduler "
  "request), it will SIGTERM the task, wait this long, and then SIGKILL. "
  "The scheduler needs this value to add into its 'safe initial wait' "
  "calculation, and into the 'minimum backoff for tasks of lost workers'. "
  "CAUTION: Therefore, if you lower this delay on a scheduler with running "
  "tasks, you will significantly increase the risk that the scheduler "
  "will start a second copy of a task before the previous one has exited."
);

namespace facebook { namespace bistro {

using namespace std;
using apache::thrift::debugString;

namespace {
template <typename... Args>
bool consensusFail(const RemoteWorker& w, Args&&... args) {
  if (!w.hasBeenHealthy()) {
    LOG(WARNING) << "Worker " << w.getBistroWorker().shard
      << " is not yet able to achieve consensus: "
      << folly::to<std::string>(std::forward<Args>(args)...);
  }
  return false;
}
}  // anonymous namespace

// A section of remote/README.worker_set_consensus documents this call.
bool RemoteWorkers::consensusPermitsBecomingHealthy(const RemoteWorker& w)
    const {
  CHECK_EQ(
    nonMustDieWorkerSetID_.hash.numWorkers, initialWorkerSetIDs_.size()
  );
  CHECK_GE(
    initialWorkerSetIDs_.size(), indirectVersionsOfNonMustDieWorkers_.size()
  );

  // Is this worker aware of every non-MUST_DIE worker?
  if (!w.workerSetID().has_value()) {
    return consensusFail(w, "It has no WorkerSetID");
  }
  // Future: think if this can be relaxed to test indirectWorkerSetID().
  if (*w.workerSetID() != nonMustDieWorkerSetID_) {
    return consensusFail(
      w, "It has WorkerSetID ", debugString(*w.workerSetID()), " while "
      "the scheduler has ", debugString(nonMustDieWorkerSetID_)
    );
  }

  // Does every non-MUST_DIE worker have an indirect version? -- we must
  // test this, since the version is set only after a worker first echoes
  // this scheduler's WorkerSetID.  If any worker lacks it, we don't have
  // a safe consensus -- that worker might declare consensus with itself.
  if (indirectVersionsOfNonMustDieWorkers_.size()
      != nonMustDieWorkerSetID_.hash.numWorkers) {
    return consensusFail(
      w, "Scheduler has ", indirectVersionsOfNonMustDieWorkers_.size(),
      " non-MUST_DIE workers with indirect versions versus ",
      nonMustDieWorkerSetID_.hash.numWorkers, " non-MUST_DIE workers"
    );
  }

  // Does each non-MUST_DIE worker's WorkerSetID indirectly require this
  // worker?  "first associated" == "first WorkerSetID containing this
  // worker", since RemoteWorkers::processHeartbeat guarantees it.
  if (!w.firstAssociatedWorkerSetID().has_value()) {
    return consensusFail(
      w, "It has not yet echoed any WorkerSetID from the scheduler"
    );
  }
  if (WorkerSetIDEarlierThan()(
    indirectVersionsOfNonMustDieWorkers_.begin()->first,
    w.firstAssociatedWorkerSetID()->version
  )) {
    return consensusFail(
      w, "It first appeared in the following WorkerSetID: ",
      w.firstAssociatedWorkerSetID()->version, " but an earlier one "
      "is indirectly required by all non-MUST_DIE workers: ",
      indirectVersionsOfNonMustDieWorkers_.begin()->first
    );
  }

  if (!w.hasBeenHealthy()) {
    LOG(INFO) << "Worker " << w.getBistroWorker().shard << " has not been "
      << "healthy, but WorkerSetID consensus allows it.";
  }
  return true;
}

namespace {
void addToVersionShardSet(
    RemoteWorkers::VersionShardSet* vss,
    const cpp2::WorkerSetID& id,
    const std::string& shard) {
  CHECK(vss->emplace(id.version, shard).second)
    << "Duplicate: v" << id.version << " in shard " << shard;
}

void removeFromVersionShardSet(
    RemoteWorkers::VersionShardSet* vss,
    const cpp2::WorkerSetID& id,
    const std::string& shard) {
  CHECK_NE(0, vss->erase({id.version, shard}))
    << "Not found: v" << id.version << " in shard " << shard;
}
}  // anonymous namespace

void RemoteWorkers::updateIndirectWorkerSetVersion(
    RemoteWorker* w,
    const cpp2::WorkerSetID& new_id) {

  auto& maybe_id = w->indirectWorkerSetID();
  CHECK(!maybe_id.has_value() || maybe_id->schedulerID == schedulerID_);
  if (maybe_id.has_value() &&
      !WorkerSetIDEarlierThan()(maybe_id->version, new_id.version)) {
    return;  // Nothing to update
  }

  const auto& bw = w->getBistroWorker();
  if (maybe_id.has_value()) {
    // Remove the current denormalized entry, since the version has changed.
    removeFromVersionShardSet(
      &indirectVersionsOfNonMustDieWorkers_, *maybe_id, bw.shard
    );
  }

  // Denormalize w's indirect version for quickly finding the smallest one.
  addToVersionShardSet(
    &indirectVersionsOfNonMustDieWorkers_, new_id, bw.shard
  );

  // Update the RemoteWorker.
  maybe_id = new_id;
}

folly::Optional<cpp2::SchedulerHeartbeatResponse>
RemoteWorkers::processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& worker,
    const cpp2::WorkerSetID& worker_set_id) {

  // It's best not to add the bad worker to the pool, so check outside of
  // RemoteWorker.  At present, we do not tell that worker to commit
  // suicide, so version mismatches will cause lots of logspam.
  enforceWorkerSchedulerProtocolVersion(
    worker.protocolVersion, cpp2::common_constants::kProtocolVersion()
  );
  const auto& shard = worker.shard;
  auto worker_it = workerPool_.find(shard);
  if (worker_it == workerPool_.end()) {

    // Even though we're adding another element, the "nextShard_"
    // round-robin iterators need not be updated, since new elements should
    // be distributed randomly throughout the hash tables.
    auto res = workerPool_.emplace(shard, std::make_shared<RemoteWorker>(
      update->curTime(),
      worker,
      worker_set_id,
      schedulerID_,
      // The following 3 callbacks maintain WorkerSetID-related state.
      //
      // New worker -- first heartbeat from this instance ID.
      [this](const RemoteWorker& w) {
        initialWorkerSetIDs_.emplace(w.initialWorkerSetID());

        // Cannot update indirectVersionsOfNonMustDieWorkers_ until
        // w.workerSetID() gets set -- wait for the worker to echo it.
        CHECK(!w.workerSetID().has_value());
        CHECK(!w.indirectWorkerSetID().has_value());
        CHECK(!w.firstAssociatedWorkerSetID().has_value());

        // Add the new worker to the non-MUST_DIE set. This would cause a
        // consensus to emerge while some workers are in the NEW state, but
        // that's not a problem because updateInitialWait() explicitly
        // prohibits leaving initial wait while any workers are NEW.
        const auto& bw = w.getBistroWorker();
        addWorkerIDToHash(&nonMustDieWorkerSetID_.hash, bw.id);
        ++nonMustDieWorkerSetID_.version;
        // Update history_ with this new set.
        HistoryStep hist_step;
        hist_step.added.emplace(bw.shard);
        CHECK(history_.emplace(
          nonMustDieWorkerSetID_.version, std::move(hist_step)
        ).second);
      },
      // Dead worker: either became MUST_DIE or got bumped by another worker.
      [this](const RemoteWorker& w) {
        auto ws_it = initialWorkerSetIDs_.find(w.initialWorkerSetID());
        CHECK(ws_it != initialWorkerSetIDs_.end());
        initialWorkerSetIDs_.erase(ws_it);

        const auto& bw = w.getBistroWorker();

        // If `w` had an indirect version, delete its denormalization.
        if (auto id_ptr = w.indirectWorkerSetID().get_pointer()) {
          CHECK(id_ptr->schedulerID == schedulerID_);
          removeFromVersionShardSet(
            &indirectVersionsOfNonMustDieWorkers_, *id_ptr, bw.shard
          );
        }

        // Remove the dead worker from the non-MUST_DIE set, update history_.
        removeWorkerIDFromHash(&nonMustDieWorkerSetID_.hash, bw.id);
        ++nonMustDieWorkerSetID_.version;
        HistoryStep hist_step;
        hist_step.removed = bw.shard;
        CHECK(history_.emplace(
          nonMustDieWorkerSetID_.version, std::move(hist_step)
        ).second);
      },
      // A worker's WorkerSetID is getting a new version (a new echo arrived).
      [this](RemoteWorker& w, const cpp2::WorkerSetID& w_set_id) {
        // Guaranteed by RemoteWorker
        CHECK(schedulerID_ == w_set_id.schedulerID)
          << debugString(schedulerID_) << " != "
          << debugString(w_set_id.schedulerID);

        // Update indirectVersionsOfNonMustDieWorkers_ and
        // w.indirectWorkerSetID_, if this update changes this worker's
        // indirect version.
        updateIndirectWorkerSetVersion(&w, w_set_id);

        // No need to update initialWorkerSetIDs_ or nonMustDieWorkerSetID_
        // or history_, since the scheduler's worker set has not changed.
      }
    ));
    // Add the same pointer to the right host worker pool
    CHECK(mutableHostWorkerPool(worker.machineLock.hostname).emplace(
      shard, res.first->second
    ).second) << "Worker pool for hostname " << worker.machineLock.hostname
      << " already had " << " shard " << shard;
    worker_it = res.first;
  }

  // If the hostname changes, we will move the worker to the new host pool
  const auto old_hostname =
    worker_it->second->getBistroWorker().machineLock.hostname;

  // Update the worker's state (also update the hostname if needed)
  auto response = worker_it->second->processHeartbeat(
    update,
    worker,
    worker_set_id,
    consensusPermitsBecomingHealthy(*worker_it->second)
  );

  // Check for hostname changes **after** handling the heartbeat, since the
  // heartbeat could have been rejected.  NB: I could also have ensured
  // res.hasValue() here, but it seems fine to just test the symptom.
  const auto& new_hostname =
    worker_it->second->getBistroWorker().machineLock.hostname;
  if (new_hostname != old_hostname) {
    // This might "invalidate" the nextShard_ iterator, but it's okay
    // since the getNextWorker() implementation is robust.
    CHECK_EQ(1, mutableHostWorkerPool(old_hostname).erase(shard))
      << "Inconsistency: did not find shard " << shard
      << " in the worker pool for its hostname " << old_hostname;
    CHECK(mutableHostWorkerPool(new_hostname).emplace(
      shard, worker_it->second
    ).second)
      << "Changing hostname " << old_hostname << " to " << new_hostname
      << ": target already had shard " << shard;
  }

  // NB: We cannot call updateInitialWait() here since it relies on knowing
  // whether any of the workers are new, and doing that here makes each
  // heartbeat take O(# workers).
  if (response.has_value()) {
    // The above callbacks maintain a key invariant: The worker itself must
    // always be part of the WorkerSetID in our reply -- this ensures that
    // RemoteWorker::firstAssociatedWorkerSetID_ is always the first
    // WorkerSetID that contains this worker.
    response->workerSetID = nonMustDieWorkerSetID_;
  }
  return response;
}

namespace {
void mergeHistoryStep(
    const RemoteWorkers::HistoryStep& step,
    std::unordered_set<std::string>* added) {
  if (step.removed.has_value()) {
    auto it = added->find(*step.removed);
    // We never merge steps from mid-history, so removed must always cancel.
    CHECK(it != added->end()) << "Worker was never added: " << *step.removed;
    added->erase(it);
  }
  for (const auto& shard : step.added) {
    CHECK(added->insert(shard).second) << "Worker exists: " << shard;
  }
}
}  // anonymous namespace

void RemoteWorkers::pruneUnusedHistoryVersions() {
  // Here is a typical timeline of a worker's first few seconds:
  //  - It delivers the first heartbeat (the WorkerSetID is typically that
  //    of another scheduler, with which it was previously associated).
  //  - The scheduler's replies with the first WorkerSetID, which contains
  //    this worker. This is to be echoed in the next heartbeat.
  //
  // Before the worker's next heartbeat arrives, the scheduler may try to
  // prune its history.  However, there is no way to prune safely at this
  // point, since the "in-flight" WorkerSetID can easily have a history
  // version which is currently unreferenced.  There is no good workaround,
  // either, since we don't want to count as referenced all the versions we
  // had ever sent in replies, and pruning those would double the code
  // complexity.  So, instead, we just postpone pruning history until all
  // current workers are established.
  CHECK_GE(
    initialWorkerSetIDs_.size(), indirectVersionsOfNonMustDieWorkers_.size()
  );
  auto limbo_workers =
    initialWorkerSetIDs_.size() - indirectVersionsOfNonMustDieWorkers_.size();
  if (limbo_workers > 0) {
    LOG(WARNING) << "Not pruning history until " << limbo_workers
      << " workers echo their first WorkerSetID from this scheduler";
    return;
  }

  folly::Optional<int64_t> first_referenced_version;
  for (const auto& p : workerPool_) {
    // It's easier to exclude MUST_DIE workers, since our other logic
    // excludes MUST_DIE, too (see the history_.clear() clause below).
    // README.worker_set_consensus explains why it is safe to exclude them.
    if (p.second->getState() == RemoteWorkerState::State::MUST_DIE) {
      continue;
    }
    if (auto wsid_ptr = p.second->workerSetID().get_pointer()) {
      // We tested for limbo workers above, so this must be true.
      CHECK(wsid_ptr);
      if (!first_referenced_version.has_value() ||
          WorkerSetIDEarlierThan()(
              wsid_ptr->version, *first_referenced_version)) {
        first_referenced_version = wsid_ptr->version;
      }
    }
  }

  if (!first_referenced_version) {
    // There are no non-MUST_DIE workers with a workerSetID(), and there are
    // no "limbo workers" (checked at start of function), so there must be
    // none at all.
    CHECK(initialWorkerSetIDs_.size() == 0);
    history_.clear();  // No non-MUST_DIE workers, no need for a history.
    return;
  }

  // Tally up all the removed / added workers from unused versions, and
  // stuff them into the first referenced version.
  std::unordered_set<std::string> added;
  for (
    // IMPORTANT: We rely on std::map's robustness to iterator invalidation.
    // The iterator type is explicit, so that this breaks on any changes to
    // the declared type of history_.
    std::map<int64_t, HistoryStep, WorkerSetIDEarlierThan>::iterator it
      = history_.begin();
    it != history_.end();
  ) {
    // This lets us safely delete cur_it. `it` might now be at .end().
    auto cur_it = it++;
    auto& p = *cur_it;
    if (WorkerSetIDEarlierThan()(p.first, *first_referenced_version)) {
      mergeHistoryStep(p.second, &added);
      // Remove the unused version entry. This is safe, since std::map's
      // iterators are stable, and we already incremented `it`.
      history_.erase(cur_it);
      // cur_it and p are now invalid, so hurry out of this loop iteration.
      continue;
    }
    // Found the first version that should not be pruned.
    CHECK(first_referenced_version == p.first) << "First referenced "
      << "WorkerSetID version was not found in the history";  // See below.
    mergeHistoryStep(p.second, &added);
    p.second.added = std::move(added);
    p.second.removed.reset();
    break;  // Unused versions pruned and merged.
  }
  // Versions are added to history_ by a RemoteWorker callback just as the
  // worker connects (and well before it echoes the new WorkerSetID back,
  // setting w.workerSetID()).  In that gap of time, the version would be at
  // risk of being pruned -- but the `limbo_workers` check at the start
  // prevents it.  Then, once w.workerSetID() is set, its version only
  // increases.  Therefore, we will never prune a version that is unused
  // now, but will be used in the future, and this check will never fire.
  CHECK(added.empty()) << "First referenced WorkerSetID "
    << " version was not found in the history";
}

// For each worker, find the highest version required by any worker in its
// indirect set.  This is one step of an iterative label propagation
// algorithm, whose goal is to lower-bound the transitive closure of
// `RemoteWorker::workerSetID` as `RemoteWorker::indirectWorkerSetID`.  This
// transitive closure is the recursive union of the closures of each of the
// workers in my `workerSetID` -- i.e. we follow all the `workerSetID`
// paths we can find that start at the current worker.  See
// README.worker_set_consensus for more details.
//
// To do this efficiently, we go through all available worker set versions
// from oldest to newest, while simultaneously walking through non-MUST_DIE
// workers from oldest `indirectWorkerSetID` to the newest.
//
// Since both traversals are sorted by version, we end up with all the
// workers that match the given history version.  We can then do one update
// step for each of the workers, potentially increasing its
// `indirectWorkerSetID`.  This is safe, since `std::map` has stable
// iterators.  As a side effect, we can end up updating a single worker
// multiple times, as increasing its `indirectWorkerSetID` will cause it to
// match a history version after the one it just matched.
void RemoteWorkers::propagateIndirectWorkerSets() {
  auto indir_it = indirectVersionsOfNonMustDieWorkers_.begin();
  // As we move forward in history, store the latest versions of the current
  // workers' indirect worker sets.  This can transiently include MUST_DIE
  // workers (they are currently MUST_DIE, but we are not yet at the step in
  // history_ where they are removed), but the `first` (version) field will
  // always be current.
  VersionShardSet vss;
  for (const auto& hp : history_) {
    // We should only rarely run out of workers -- the latest version would
    // have to be unreferenced by any worker, meaning that a worker `w` just
    // became MUST_DIE, and no other workers's `indirectWorkerSetID` has
    // caught up to the version where `w` became MUST_DIE.
    if (indir_it == indirectVersionsOfNonMustDieWorkers_.end()) {
      break;  // No more workers to update.
    }

    CHECK(!WorkerSetIDEarlierThan()(indir_it->first, hp.first))
      << "Worker refers to version not in history -- history v" << hp.first
      << " came before v" << indir_it->first << " from " << indir_it->second;

    // Compute the highest indirect version referenced by workers in the
    // current history step.  Note that this will propagate workerSetID
    // dependencies through MUST_DIE workers, but this is harmless as per
    // the note in README.worker_set_consensus.
    if (auto shard_p = hp.second.removed.get_pointer()) {  // Remove, then add
      const auto& w = *mutableWorkerOrAbort(*shard_p);  // Look up shard
      // It is harmless to leave out from `vss` these "in-limbo" workers
      // that have not yet received their first WorkerSetID from this
      // scheduler.  Firstly, we are, propagating *conservative* estimates
      // of indirect WorkerSetID versions -- and if we magically knew the
      // value for this worker, it could at best *increase* the maximum
      // version in `vss`.  Secondly, once the worker gets a version, the
      // next iteration will propagate it properly, fixing the omission.
      if (w.indirectWorkerSetID().has_value()) {
        CHECK(w.indirectWorkerSetID()->schedulerID == schedulerID_);
        removeFromVersionShardSet(&vss, *w.indirectWorkerSetID(), *shard_p);
      }
    }
    for (const auto& shard : hp.second.added) {
      const auto& w = *mutableWorkerOrAbort(shard);  // Look up shard
      if (!w.indirectWorkerSetID().has_value()) {
        continue;  // See comment above
      }
      CHECK(w.indirectWorkerSetID()->schedulerID == schedulerID_);
      addToVersionShardSet(&vss, *w.indirectWorkerSetID(), shard);
    }
    // For all workers having the current version (from `hp`) as their
    // `indirectWorkerSet`, replace this set with the highest-versioned
    // `indirectWorkerSet` in `vss`.
    while (
      indir_it != indirectVersionsOfNonMustDieWorkers_.end() &&
      !WorkerSetIDEarlierThan()(hp.first, indir_it->first)
    ) {
      // Empty vss means that this is a version in history with *no* workers
      // connected.  A worker can clearly never get such a version as its
      // workerSetID(), which means that when vss is empty, there must be no
      // workers to update and we don't enter the loop.
      CHECK(!vss.empty());
      // We never delete intermediate versions from history_
      CHECK_EQ(hp.first, indir_it->first);

      auto it = indir_it;  // Only use `it` and not `indir_it` below.
      ++indir_it;  // So we can safely remove `it`.

      // Get a WorkerSetID for the highest-version `indirectWorkerSet`
      // assigned to any worker at the current step in history (`hp`).  The
      // easiest way to look up that worker is by shard.
      auto new_id =
        mutableWorkerOrAbort(vss.rbegin()->second)->indirectWorkerSetID();
      // Can't end up in `vss` without having an indirectWorkerSetID version.
      CHECK(new_id.has_value());
      CHECK_EQ(vss.rbegin()->first, new_id->version);
      CHECK(new_id->schedulerID == schedulerID_);

      // Find the worker to update.
      auto& w = *mutableWorkerOrAbort(it->second);
      // Can't end up in indirectVersionsOfNonMustDieWorkers_ without having
      // an indirectWorkerSetID version.
      CHECK_EQ(it->first, w.indirectWorkerSetID()->version);
      CHECK_EQ(it->second, w.getBistroWorker().shard);
      // Since we're about to update `w`, we must also do `vss` -- the `vss`
      // update step above searches for the latest `indirectWorkerSetID`.
      auto vss_it = vss.find({it->first, it->second});
      if (vss_it != vss.end()) {
        vss.erase(vss_it);
        addToVersionShardSet(&vss, *new_id, it->second);
      }

      // Propagate to the current worker the highest-version
      // `indirectWorkerSetID` of all the workers in its current
      // `indirectWorkerSetID`.  Also updates
      // `indirectVersionsOfNonMustDieWorkers_`
      //
      // CAUTION: This invalidates `it`.
      updateIndirectWorkerSetVersion(&w, *new_id);
      CHECK(new_id->version == w.indirectWorkerSetID()->version)
        << debugString(new_id->version) << " != "
        << debugString(w.indirectWorkerSetID()->version);
    }
  }
  CHECK(indir_it == indirectVersionsOfNonMustDieWorkers_.end())
    << "indirectWorkerSetID version " << indir_it->first << " in shard "
    << indir_it->second << " exceeds the maximum history version of "
    << (history_.empty() ? -1 : history_.rbegin()->first);
}

void RemoteWorkers::updateInitialWait(RemoteWorkerUpdate* update) {
  // Future: Maybe remove everything in update->suicideWorkers_ from the
  // worker pools?

  std::string msg;
  if (!inInitialWait_) {
    update->setInitialWaitMessage(std::move(msg));
    return;
  }

  const time_t kMinSafeWait =
    RemoteWorkerState::maxHealthcheckGap() +
    RemoteWorkerState::loseUnhealthyWorkerAfter() +
    RemoteWorkerState::workerCheckInterval() +  // extra safety gap
    RemoteWorkerState::workerSuicideBackoffSafetyMarginSec() +
    (RemoteWorkerState::workerSuicideTaskKillWaitMs() / 1000) + 1;

  time_t min_start_time = update->curTime();
  if (FLAGS_CAUTION_startup_wait_for_workers < 0) {
    min_start_time -= kMinSafeWait;
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
    } else if (kMinSafeWait > FLAGS_CAUTION_startup_wait_for_workers) {
      msg += folly::to<std::string>(
        "Your custom --CAUTION_startup_wait_for_workers is ",
        "less than the minimum safe value of ", kMinSafeWait,
        " -- this increases the risk of starting second copies of tasks ",
        "that were already running. "
      );
    }
  }

  // Are exactly the same workers connected to the scheduler now, as before
  // the restart?
  bool initial_worker_set_id_consensus =
    // The initial worker set ID is the same for all non-MUST_DIE workers,
    initialWorkerSetIDs_.end()
      == initialWorkerSetIDs_.upper_bound(*initialWorkerSetIDs_.begin())
    // ... and it matches our non-MUST_DIE worker set, meaning that exactly
    // the same workers are connected now as the scheduler had before its
    // restart.
    && nonMustDieWorkerSetID_.hash == initialWorkerSetIDs_.begin()->hash;
  if (!initial_worker_set_id_consensus) {
    msg += "No initial worker set ID consensus. ";
  }

  // The scheduler is eligible to exit initial wait if:
  //  (i) there are no NEW workers, AND
  //  (ii) --min_startup_wait_for_workers has expired, AND
  //  (iii) EITHER the wait expired, OR all connected workers have the same
  //        initial WorkerSetID, which matches the non-MUST_DIE worker set.
  //
  // If the wait expires, we deliberately do not wait for the WorkerSetID
  // consensus, for two reasons.  Firstly, people "who know what they are
  // doing" need to be able to manually shorten the initial wait.  Secondly,
  // if the initial wait is safe, there is really no benefit to waiting for
  // the consensus -- but it *can* needlessly slow down startup if some
  // workers become unhealthy.
  if (min_start_time < startTime_ && !initial_worker_set_id_consensus) {
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
  // It shouldn't matter much whether we prune or propagate first, but doing
  // it before updateState() means that workers should get healthy faster.
  pruneUnusedHistoryVersions();
  propagateIndirectWorkerSets();
  for (auto& pair : workerPool_) {
    pair.second->updateState(
      update, consensusPermitsBecomingHealthy(*pair.second)
    );
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
