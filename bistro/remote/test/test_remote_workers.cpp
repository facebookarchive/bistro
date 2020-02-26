/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>
#include <folly/Random.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkers.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/remote/WorkerSetID.h"
#include "bistro/bistro/statuses/TaskStatus.h"

using namespace facebook::bistro;
using namespace folly::test;

using apache::thrift::debugString;

using IDSet = std::set<cpp2::BistroInstanceID>;

DECLARE_int32(CAUTION_startup_wait_for_workers);

void printString(folly::StringPiece s) {
  if (!s.empty()) {
    std::cout << "stderr: " << s << std::flush;
  }
}

cpp2::BistroInstanceID randInstanceID() {
  cpp2::BistroInstanceID id;
  id.startTime = folly::Random::rand64();
  id.rand = folly::Random::rand64();
  return id;
}

cpp2::BistroWorker makeWorker(std::string shard) {
  cpp2::BistroWorker worker;
  worker.shard = shard;
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  worker.id = randInstanceID();
  return worker;
}

cpp2::WorkerSetID workerSetID(
    const RemoteWorkers& r,
    int64_t v,
    const cpp2::WorkerSetHash& h) {
  cpp2::WorkerSetID id;
  id.hash = h;
  id.version = v;
  id.schedulerID = r.nonMustDieWorkerSetID().schedulerID;
  return id;
}

TEST(TestRemoteWorkers, ProtocolMismatch) {
  auto worker = makeWorker("w");
  RemoteWorkerUpdate update;
  auto scheduler_id = randInstanceID();
  RemoteWorkers r(0, scheduler_id);

  // Mismatched version
  worker.protocolVersion = -1;
  EXPECT_THROW(
    r.processHeartbeat(&update, worker, cpp2::WorkerSetID()),
    std::runtime_error
  );
  EXPECT_TRUE(r.workerPool().begin() == r.workerPool().end());

  // Matched version
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  auto res = r.processHeartbeat(&update, worker, cpp2::WorkerSetID());
  EXPECT_TRUE(res.has_value());
  cpp2::WorkerSetHash wh;
  addWorkerIDToHash(&wh, worker.id);
  EXPECT_EQ(workerSetID(r, 1, wh), res->workerSetID);
  EXPECT_FALSE(r.workerPool().begin() == r.workerPool().end());
  EXPECT_TRUE(++r.workerPool().begin() == r.workerPool().end());
  EXPECT_NE(nullptr, r.getWorker(worker.shard));
}

IDSet dredgeHostPool(
    RemoteWorkers& r,
    std::string host,
    size_t max_pool_size = 10) {

  IDSet ids;
  for (size_t i = 0; i < max_pool_size + 1; ++i) {
    ids.emplace(r.getNextWorkerByHost(host)->getBistroWorker().id);
  }
  return ids;
}

TEST(TestRemoteWorkers, WorkerPools) {
  // This test mostly does not inspect `update`, since that is supposed to
  // be exhaustively tested by test_remote_worker.cpp.
  RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 0);
  RemoteWorkers r(0, randInstanceID());

  // 3 workers on 2 hosts
  auto w1 = makeWorker("w1");
  w1.machineLock.hostname = "host1";
  w1.machineLock.port = 123;

  auto w2 = makeWorker("w2");
  w2.machineLock.hostname = "host1";
  w2.machineLock.port = 456;

  auto w3 = makeWorker("w3");
  w3.machineLock.hostname = "host2";
  w3.machineLock.port = 123;

  cpp2::WorkerSetHash wh;

  // getNextWorkerByHost
  {
    auto res = r.processHeartbeat(&update, w1, cpp2::WorkerSetID());
    EXPECT_EQ(
      static_cast<int>(RemoteWorkerState::State::NEW), res->workerState
    );
    addWorkerIDToHash(&wh, w1.id);
    EXPECT_EQ(workerSetID(r, 1, wh), res->workerSetID);
    EXPECT_EQ(IDSet{w1.id}, dredgeHostPool(r, "host1"));

    res = r.processHeartbeat(&update, w2, cpp2::WorkerSetID());
    EXPECT_EQ(
      static_cast<int>(RemoteWorkerState::State::NEW), res->workerState
    );
    addWorkerIDToHash(&wh, w2.id);
    EXPECT_EQ(workerSetID(r, 2, wh), res->workerSetID);
    EXPECT_EQ(IDSet({w1.id, w2.id}), dredgeHostPool(r, "host1"));

    res = r.processHeartbeat(&update, w3, cpp2::WorkerSetID());
    EXPECT_EQ(
      static_cast<int>(RemoteWorkerState::State::NEW), res->workerState
    );
    addWorkerIDToHash(&wh, w3.id);
    EXPECT_EQ(workerSetID(r, 3, wh), res->workerSetID);
    EXPECT_EQ(IDSet({w1.id, w2.id}), dredgeHostPool(r, "host1"));
    EXPECT_EQ(IDSet({w3.id}), dredgeHostPool(r, "host2"));
  }

  auto check_all_workers_fn = [&](std::vector<cpp2::BistroWorker> workers) {
    IDSet expected_ids;

    // getWorker
    for (const auto& w : workers) {
      EXPECT_EQ(w.id, r.getWorker(w.shard)->getBistroWorker().id);
      expected_ids.emplace(w.id);
    }

    // getNextWorker
    IDSet all_ids;
    for (size_t i = 0; i < 4; ++i) {
      all_ids.emplace(r.getNextWorker()->getBistroWorker().id);
    }
    EXPECT_EQ(expected_ids, all_ids);

    // iterator
    IDSet iter_ids;
    for (const auto& rw : r.workerPool()) {
      iter_ids.emplace(rw.second->getBistroWorker().id);
    }
    EXPECT_EQ(expected_ids, iter_ids);
  };
  check_all_workers_fn(std::vector<cpp2::BistroWorker>({w1, w2, w3}));

  // Try and fail to replace w1 with another worker before w1 is lost.
  {
    auto check_worker_pools_unchanged_fn = [&]() {
      EXPECT_EQ(IDSet({w1.id, w2.id}), dredgeHostPool(r, "host1"));
      EXPECT_EQ(IDSet({w3.id}), dredgeHostPool(r, "host2"));
      check_all_workers_fn(std::vector<cpp2::BistroWorker>({w1, w2, w3}));
    };

    auto w1new = makeWorker("w1");
    w1new.machineLock.hostname = "host2";
    w1new.machineLock.port = 789;
    ASSERT_NE(w1.id, w1new.id);

    // w1new's heartbeat was rejected, and we ask it to commit suicide
    {
      RemoteWorkerUpdate update2(
        RemoteWorkerUpdate::UNIT_TEST_TIME,
        FLAGS_lose_unhealthy_worker_after - 1  // not long enough to lose w1
      );
      EXPECT_FALSE(
          r.processHeartbeat(&update2, w1new, cpp2::WorkerSetID()).has_value());
      EXPECT_EQ(1, update2.suicideWorkers().size());
      EXPECT_EQ(w1new.id, update2.suicideWorkers().begin()->second.id);
      EXPECT_EQ(0, update2.newWorkers().size());
      check_worker_pools_unchanged_fn();
    }

    // w1's heartbeat is still received OK (we used to hit a CHECK here)
    {
      RemoteWorkerUpdate update2(
        RemoteWorkerUpdate::UNIT_TEST_TIME,
        FLAGS_lose_unhealthy_worker_after - 1
      );
      auto res = r.processHeartbeat(&update2, w1, cpp2::WorkerSetID());
      EXPECT_EQ(
        static_cast<int>(RemoteWorkerState::State::NEW), res->workerState
      );
      EXPECT_EQ(workerSetID(r, 3, wh), res->workerSetID);  // No change
      EXPECT_EQ(0, update2.suicideWorkers().size());
      EXPECT_EQ(1, update2.newWorkers().size());
      EXPECT_EQ(w1.id, update2.newWorkers().begin()->second.id);
      check_worker_pools_unchanged_fn();
    }
  }

  // Successfully move a worker from one host to another
  {
    auto w1new = makeWorker("w1");
    w1new.machineLock.hostname = "host2";
    w1new.machineLock.port = 789;
    ASSERT_NE(w1.id, w1new.id);
    {
      RemoteWorkerUpdate update2(
        RemoteWorkerUpdate::UNIT_TEST_TIME,
        FLAGS_lose_unhealthy_worker_after + 1  // long enough to lose w1
      );

      auto res = r.processHeartbeat(&update2, w1new, cpp2::WorkerSetID());
      EXPECT_EQ(
        static_cast<int>(RemoteWorkerState::State::NEW), res->workerState
      );
      removeWorkerIDFromHash(&wh, w1.id);
      addWorkerIDToHash(&wh, w1new.id);
      // 1 deletion, 1 addition == skip 2 versions
      auto wsid = workerSetID(r, 5, wh);
      EXPECT_EQ(wsid, res->workerSetID)
        << debugString(wsid) << " != " << debugString(res->workerSetID);

      // Sanity check: w1new should have bumped w1
      EXPECT_EQ(1, update2.suicideWorkers().size());
      EXPECT_EQ(w1.id, update2.suicideWorkers().begin()->second.id);
      EXPECT_EQ(1, update2.newWorkers().size());
      EXPECT_EQ(w1new.id, update2.newWorkers().begin()->second.id);
    }
    EXPECT_EQ(IDSet({w2.id}), dredgeHostPool(r, "host1"));
    EXPECT_EQ(IDSet({w1new.id, w3.id}), dredgeHostPool(r, "host2"));
    check_all_workers_fn(std::vector<cpp2::BistroWorker>({w1new, w2, w3}));
  }
}

struct TestRemoteWorkersInitialWait : public ::testing::Test {
  TestRemoteWorkersInitialWait() {
    // Known values for more predictable tests
    FLAGS_CAUTION_startup_wait_for_workers = -1;
    FLAGS_worker_check_interval = 1;
    FLAGS_healthcheck_period = 3;
    FLAGS_healthcheck_grace_period = 9;
    FLAGS_lose_unhealthy_worker_after = 27;
    FLAGS_CAUTION_worker_suicide_backoff_safety_margin_sec = 81;
    FLAGS_CAUTION_worker_suicide_task_kill_wait_ms = 243000;
  }
};

// Check the timeouts with no workers.
TEST_F(TestRemoteWorkersInitialWait, NoWorkers) {
  const time_t kSafeInitialWait =
    27 +  // lose_unhealthy_worker_after
    // maxHealthcheckGap: _grace_period, _period, worker_check_interval
    (9 + 3 + 1) +
    1 +  // worker_check_interval
    81 +  // CAUTION_worker_suicide_backoff_safety_margin_sec
    243 + 1;  // CAUTION_worker_suicide_task_kill_wait_ms

  RemoteWorkers r(0, randInstanceID());
  // The last 3 shows that once the wait is over, it can't be turned back on.
  for (time_t t : std::vector<time_t>{
    0, 1, 5, kSafeInitialWait - 2, kSafeInitialWait - 1, kSafeInitialWait, 3
  }) {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, t);
    r.updateState(&update);
    if (t == kSafeInitialWait || t == 3) {
      EXPECT_EQ("", update.initialWaitMessage());  // Wait expired
    } else {
      EXPECT_NO_PCRE_MATCH(".*CAUTION.*", update.initialWaitMessage());
      EXPECT_PCRE_MATCH(".*Waiting for all.*", update.initialWaitMessage());
    }
  }
}

// Check timeouts with one worker and no consensus (e.g. it's new). Also
// check that we wait to get its running tasks, since it's very risky to
// start new tasks when you have a known & somewhat responsive worker that
// may have some unknown tasks.
TEST_F(TestRemoteWorkersInitialWait, OneWorkerWithRunningTasks) {
  // I want to play the game of "the worker remains in NEW status when the
  // initial wait expires".  A safe initial wait is set up so that such
  // workers will commit suicide first, so use an unsafe one.
  const int kInitialWait = 10;
  FLAGS_CAUTION_startup_wait_for_workers = kInitialWait;
  RemoteWorkers r(0, randInstanceID());
  auto w = makeWorker("w");
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 0);
    r.processHeartbeat(&update, w, cpp2::WorkerSetID());
    // processHeartbeat does not set the initial wait message.
    EXPECT_EQ("unknown", update.initialWaitMessage());
  }
  // Going back to 7 confirms we remained in "NEW" state.
  for (time_t t : std::vector<time_t>{0, 5, kInitialWait, 7}) {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, t);
    r.updateState(&update);
    EXPECT_PCRE_MATCH(".*CAUTION.*", update.initialWaitMessage());
    if (t == kInitialWait) {
      EXPECT_PCRE_MATCH(
        ".* Ready to exit initial wait, but .* not allowing tasks to start .*",
        update.initialWaitMessage()
      );
    } else {
      EXPECT_PCRE_MATCH(".*Waiting for all.*", update.initialWaitMessage());
    }
  }
  // We exit initial wait once the worker's running tasks are supplied.
  RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, kInitialWait);
  r.initializeRunningTasks(w, {});
  r.updateState(&update);
  EXPECT_EQ("", update.initialWaitMessage());  // Wait expired
}

// Copy-pasta'd from test_remote_worker.cpp
void successfulHealthcheck(int64_t t, RemoteWorkers& r, std::string shard) {
  cpp2::RunningTask rt;
  rt.workerShard = shard;
  rt.job = kHealthcheckTaskJob;
  rt.invocationID.startTime = t;
  auto w = r.mutableWorkerOrAbort(shard);
  EXPECT_FALSE(w->recordNonRunningTaskStatus(
    rt, TaskStatus::done(), w->getBistroWorker().id
  ));
}

/**
 * Add the worker, and get it out of NEW state, while monitoring the
 * consensus variables-related variables.  Uses the timestamp '1'.  The
 * resulting worker would be healthy if consensus permits.
 */
void addWorker(
    CaptureFD* fd,
    RemoteWorkers* r,
    const cpp2::BistroWorker& w,
    const cpp2::WorkerSetID& initial_id,
    const std::multiset<cpp2::WorkerSetID>& expected_initial_ids,
    const cpp2::WorkerSetID& expected_non_mustdie_id) {

  auto no_consensus_re = folly::to<std::string>(
    ".* ", w.shard, " can be healthy but lacks WorkerSetID consensus.*"
  );

  for (int i = 0; i < 2; ++i) {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    // First pass: register the worker, and fake a successful healthcheck.
    if (i == 0) {
      auto res = r->processHeartbeat(&update, w, initial_id);
      EXPECT_EQ(expected_non_mustdie_id, res->workerSetID);
      successfulHealthcheck(1, *r, w.shard);
    // Second pass: run an updateState so that we can check the initial wait
    // message, and ensure nothing else changes.
    } else {
      // We're not supposed to prune history when any workers are NEW.
      std::string not_pruning_re(".*Not pruning history until.*");
      auto out = fd->readIncremental();
      EXPECT_NO_PCRE_MATCH(no_consensus_re, out);
      EXPECT_NO_PCRE_MATCH(not_pruning_re, out);
      r->updateState(&update);
      out = fd->readIncremental();
      EXPECT_NO_PCRE_MATCH(no_consensus_re, out);
      EXPECT_PCRE_MATCH(not_pruning_re, out);
      EXPECT_PCRE_MATCH(".*Waiting for all.*", update.initialWaitMessage());
    }
    auto rw = r->getWorker(w.shard);
    EXPECT_EQ(RemoteWorkerState::State::NEW, rw->getState());
    EXPECT_EQ(initial_id, rw->initialWorkerSetID());
    EXPECT_FALSE(rw->workerSetID().has_value());
    // This is set the first time the WorkerSetID changes from the initial.
    EXPECT_FALSE(rw->firstAssociatedWorkerSetID().has_value());
    EXPECT_EQ(expected_non_mustdie_id, r->nonMustDieWorkerSetID());
    EXPECT_EQ(expected_initial_ids, r->initialWorkerSetIDs());
  }

  // Once the worker fetches running tasks, it's ready to become healthy.
  r->initializeRunningTasks(w, {});
  EXPECT_EQ(
    RemoteWorkerState::State::UNHEALTHY, r->getWorker(w.shard)->getState()
  );

  EXPECT_NO_PCRE_MATCH(no_consensus_re, fd->readIncremental());
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    // Coverage assertion: we're about to propagate with some workers
    // lacking an indirectWorkerSetID, and that works fine.
    EXPECT_FALSE(r->getWorker(w.shard)->indirectWorkerSetID().has_value());
    r->updateState(&update);
    EXPECT_PCRE_MATCH(".*Waiting for all.*", update.initialWaitMessage());
  }
  EXPECT_PCRE_MATCH(no_consensus_re, fd->readIncremental());
}

// A "script-style" test wherein 2 workers leave initial wait early by
// achieving consensus, interrupted by an impostor worker briefly joining.
TEST_F(TestRemoteWorkersInitialWait, AchieveAndMaintainWorkerSetConsensus) {
  CaptureFD stderr(2, printString);

  RemoteWorkers r(0, randInstanceID());
  auto w1 = makeWorker("w1");
  auto w2 = makeWorker("w2");
  auto w3 = makeWorker("w3");

  cpp2::WorkerSetID id2;  // 1) The first worker.
  addWorkerIDToHash(&id2.hash, w2.id);

  cpp2::WorkerSetID id3;  // 2) This impostor joins before we reach consensus.
  addWorkerIDToHash(&id3.hash, w3.id);

  cpp2::WorkerSetID id23 = id2;
  addWorkerIDToHash(&id23.hash, w3.id);

  cpp2::WorkerSetID id12 = id2;  // 4) The consensus, once the impostor leaves.
  addWorkerIDToHash(&id12.hash, w1.id);

  cpp2::WorkerSetID id123 = id12;  // 3) All three together.
  addWorkerIDToHash(&id123.hash, w3.id);

  addWorker(
    &stderr, &r, w2, id12, {id12}, workerSetID(r, 1, id2.hash)
  );
  addWorker(
    &stderr, &r, w3, id3, {id12, id3}, workerSetID(r, 2, id23.hash)
  );
  addWorker(
    &stderr, &r, w1, id12, {id12, id12, id3}, workerSetID(r, 3, id123.hash)
  );
  // These workers are unhealthy since they don't know about each other, but
  // for the purposes of this test, that does not matter.

  // Now, lose w3, and thus achieve consensus. Use heartbeats to this
  // effect in order to avoid messing with the other workers.
  int64_t t = 1;
  for (auto& p : std::vector<std::pair<RemoteWorkerState::State, int64_t>>{
    // Already unhealthy. This pass just tests "ignore WorkerSetIDs from the
    // wrong worker".
    std::make_pair(RemoteWorkerState::State::UNHEALTHY, 1),
    // Lose the worker
    std::make_pair(
      RemoteWorkerState::State::MUST_DIE, FLAGS_lose_unhealthy_worker_after
    ),
  }) {
    t += p.second;
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, t);

    // This RemoteWorkers test is rare that it processes a second heartbeat
    // on the same RemoteWorker, letting us do some extra WorkerSetID chescks
    auto res = r.processHeartbeat(
      &update, w3, p.first == RemoteWorkerState::State::UNHEALTHY
        ? id3  // First pass: from the wrong scheduler, will be ignored.
        // Second pass: this is what would actually be echoed by the worker.
        : r.nonMustDieWorkerSetID()
    );

    auto maybe_id = r.getWorker("w3")->firstAssociatedWorkerSetID();
    if (p.first == RemoteWorkerState::State::UNHEALTHY) {
      EXPECT_EQ(workerSetID(r, 3, id123.hash), res->workerSetID);
      ASSERT_FALSE(maybe_id.has_value());
    } else {
      EXPECT_EQ(workerSetID(r, 4, id12.hash), res->workerSetID);
      EXPECT_EQ(workerSetID(r, 3, id123.hash), maybe_id);
    }
    EXPECT_EQ(p.first, r.getWorker("w3")->getState());
  }
  EXPECT_EQ(workerSetID(r, 4, id12.hash), r.nonMustDieWorkerSetID());
  EXPECT_EQ(
    (std::multiset<cpp2::WorkerSetID>{id12, id12}), r.initialWorkerSetIDs()
  );

  // Consensus was achieved, so we exit initial wait.
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    r.updateState(&update);
    EXPECT_EQ("", update.initialWaitMessage());
  }
  EXPECT_NO_PCRE_MATCH(glogErrorPattern(), stderr.readIncremental());
}

// It is important that the default WorkerSetID (from a worker that never
// had a scheduler) does not trigger consensus.
TEST_F(TestRemoteWorkersInitialWait, NoConsensusWithEmptyWorkerSetID) {
  CaptureFD stderr(2, printString);

  RemoteWorkers r(0, randInstanceID());
  auto w = makeWorker("w");
  cpp2::WorkerSetHash h;
  addWorkerIDToHash(&h, w.id);
  cpp2::WorkerSetID id;  // empty initial WorkerSetID
  addWorker(&stderr, &r, w, id, {id}, workerSetID(r, 1, h));

  // There is no consensus, so we remain in initial wait.
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    r.updateState(&update);
    EXPECT_NE("", update.initialWaitMessage());
  }
  EXPECT_NO_PCRE_MATCH(glogErrorPattern(), stderr.readIncremental());
}

void updateWorkers(
    RemoteWorkers* r,
    const RemoteWorkers::VersionShardSet& vss,
    const std::map<std::string, cpp2::WorkerSetID>& indirect_wsids,
    const RemoteWorkers::History& history,
    // After the update
    const std::map<std::string, bool>& shard_to_consensus_permits) {

  EXPECT_EQ(vss, r->indirectVersionsOfNonMustDieWorkers());
  for (const auto& p : indirect_wsids) {
    EXPECT_EQ(p.second, r->getWorker(p.first)->indirectWorkerSetID())
      << p.first << ": " << debugString(p.second) << " != "
      << debugString(*r->getWorker(p.first)->indirectWorkerSetID());
  }
  EXPECT_EQ(history, r->historyForUnitTest());
  RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
  r->updateState(&update);

  for (const auto& p : shard_to_consensus_permits) {
    if (p.second) {
      EXPECT_TRUE(r->consensusPermitsBecomingHealthyForUnitTest(p.first));
    } else {
      EXPECT_FALSE(r->consensusPermitsBecomingHealthyForUnitTest(p.first));
    }
  }
}

void addAndUpdateWorker(
    RemoteWorkers* r,
    const cpp2::BistroWorker& w,
    const cpp2::WorkerSetID& wid,
    size_t num_workers,  // After we add this one
    const RemoteWorkers::VersionShardSet& before_vss,
    const RemoteWorkers::VersionShardSet& after_vss,
    const std::map<std::string, cpp2::WorkerSetID>& indirect_wsids,
    const RemoteWorkers::History& history,
    // After propagation
    const std::map<std::string, bool>& shard_to_consensus_permits) {
  cpp2::WorkerSetID empty_wid;  // Used for workers' initial WorkerSetIDs
  std::multiset<cpp2::WorkerSetID> expect_initial_wsids;
  for (size_t i = 0; i < num_workers; ++i) {
    expect_initial_wsids.insert(empty_wid);
  }
  {
    CaptureFD stderr(2, printString);
    addWorker(&stderr, r, w, empty_wid, expect_initial_wsids, wid);
  }
  EXPECT_EQ(before_vss, r->indirectVersionsOfNonMustDieWorkers());

  EXPECT_EQ(num_workers - 1, r->indirectVersionsOfNonMustDieWorkers().size());
  EXPECT_EQ(num_workers, r->nonMustDieWorkerSetID().hash.numWorkers);
  // All are blocked since the above two numbers don't match:
  for (const auto& p : shard_to_consensus_permits) {
    EXPECT_FALSE(r->consensusPermitsBecomingHealthyForUnitTest(p.first));
  }

  // Need one more heartbeat for w to get a workerSetID.
  EXPECT_FALSE(r->getWorker(w.shard)->workerSetID().has_value());
  EXPECT_FALSE(r->getWorker(w.shard)->indirectWorkerSetID().has_value());
  EXPECT_FALSE(r->getWorker(w.shard)->firstAssociatedWorkerSetID().has_value());
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    EXPECT_EQ(wid, r->processHeartbeat(&update, w, wid)->workerSetID);
  }
  EXPECT_EQ(wid, r->getWorker(w.shard)->workerSetID());
  EXPECT_EQ(wid, r->getWorker(w.shard)->indirectWorkerSetID());
  EXPECT_EQ(wid, r->getWorker(w.shard)->firstAssociatedWorkerSetID());

  EXPECT_EQ(num_workers, r->indirectVersionsOfNonMustDieWorkers().size());
  EXPECT_EQ(num_workers, r->nonMustDieWorkerSetID().hash.numWorkers);

  // No updateState has happened since the worker acquired a WorkerSetID, so
  // test twice (before and after the update) -- nothing changes.
  for (int i = 0; i < 2; ++i) {
    updateWorkers(
      r, after_vss, indirect_wsids, history, shard_to_consensus_permits
    );
  }
}

void checkWorkerSetIDs(
    const RemoteWorkers& r,
    const std::map<std::string, cpp2::WorkerSetID>& shard_to_id) {
  for (const auto& p : shard_to_id) {
    EXPECT_EQ(p.second, r.getWorker(p.first)->workerSetID());
  }
}

/**
 * This is a "script"-style test, which executes a carefully choreographed
 * and deterministic sequence of steps between RemoteWorkers and individual
 * workers.
 *
 * It explicitly ignores the following aspects of RemoteWorkers, which are
 * tested separately:
 *  - initial worker set IDs (they are set so as to avoid consensus)
 *  - health status (workers are never made to become healthy)
 *  - initial wait logic -- we never leave initial wait
 *
 * The goal is to cover:
 *  - consensusPermitsBecomingHealthy(RemoteWorker)
 *  - indirectVersionsOfNonMustDieWorkers_ (including version propagation)
 *  - history_ (including adding/removing workers, pruning history steps)
 *  - mutation of RemoteWorker::indirectWorkerSetID()
 *
 * It also covers updates to nonMustDieWorkerSetID_ via addWorker().
 */
TEST_F(TestRemoteWorkersInitialWait, HistoryAndWorkerSetIDPropagation) {
  RemoteWorkers r(0, randInstanceID());
  auto w1 = makeWorker("w1");
  auto w2 = makeWorker("w2");
  auto w3 = makeWorker("w3");
  auto w4 = makeWorker("w4");

  cpp2::WorkerSetHash hash1;
  addWorkerIDToHash(&hash1, w1.id);
  auto hash12 = hash1;
  addWorkerIDToHash(&hash12, w2.id);
  auto hash123 = hash12;
  addWorkerIDToHash(&hash123, w3.id);
  auto hash1234 = hash123;
  addWorkerIDToHash(&hash1234, w4.id);
  auto hash234 = hash1234;
  removeWorkerIDFromHash(&hash234, w1.id);

  RemoteWorkers::History history;

  ///
  /// Add w1. Make sure w1's workerSetID becomes {w1}, then do not give it
  /// more updates.  Propagate / prune, observe the desired outcomes.
  ///

  auto wid1 = workerSetID(r, 1, hash1);
  history.insert({1, {folly::none, {w1.shard}}});
  addAndUpdateWorker(
    &r,
    w1,
    wid1,
    1,
    // Denormalized versions of indirectWorkerSetID change as expected.
    {},
    {{1, "w1"}},
    {},  // No indirectWorkerSetIDs to verify.
    history,
    // consensus permits w1 to be healthy since it knows about itself.
    {{"w1", true}}
  );

  ///
  /// Add w2. Ensure w2's workerSetID becomes {w1, w2}.
  /// Propagate / prune, observe the desired outcomes, especially:
  ///  * w1's indirect version is not updated (nor its denorm)
  ///

  auto wid12 = workerSetID(r, 2, hash12);
  history.insert({2, {folly::none, {w2.shard}}});
  addAndUpdateWorker(
    &r,
    w2,
    wid12,
    2,
    // Denormalized versions of indirectWorkerSetID change as expected.
    {{1, "w1"}},
    {{1, "w1"}, {2, "w2"}},
    {{"w1", wid1}},  // w1's indirect WorkerSetIDs is as before.
    history,
    // Almost all the conditions for w1 & w2 to be allowed to be
    // healthy are met, but we're still blocked on them failing to
    // indirectly require each other.
    {{"w1", false}, {"w2", false}}
  );

  ///
  /// Add w3. Ensure w3's workerSetID becomes {w1, w2, w3}.
  /// Propagate / prune, observe desired outcome, especially:
  ///  * w1 & w2's indirect versions not updated (nor denorm)
  ///

  auto wid123 = workerSetID(r, 3, hash123);
  history.insert({3, {folly::none, {w3.shard}}});
  addAndUpdateWorker(
    &r,
    w3,
    wid123,
    3,
    // Denormalized versions of indirectWorkerSetID change as expected.
    {{1, "w1"}, {2, "w2"}},
    {{1, "w1"}, {2, "w2"}, {3, "w3"}},
    {{"w1", wid1}, {"w2", wid12}},  // Indirect WorkerSetIDs are as before.
    history,
    // Almost all the conditions for w1 & w2 & w3 to be allowed to be
    // healthy are met, but we're still blocked on them failing to
    // indirectly require each other.
    {{"w1", false}, {"w2", false}, {"w3", false}}
  );

  ///
  /// Ensure w2's workerSetID() becomes {w1, w2, w3}
  ///  * w2's indirect version **is** immediately updated (and its denorm)
  /// Propagate / prune, observe desired outcome, especially:
  ///  * no workers' indirect versions are updated (nor the denorms)
  ///

  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    EXPECT_EQ(wid123, r.processHeartbeat(&update, w2, wid123)->workerSetID);
  }
  EXPECT_EQ(wid123, r.getWorker("w2")->workerSetID());

  // No updateState has happened since w2's second heartbeat, so test twice
  // (before and after the update) -- nothing changes.
  for (int i = 0; i < 2; ++i) {
    updateWorkers(
      &r,
      {{1, "w1"}, {3, "w2"}, {3, "w3"}},  // w2's version just got updated
      // w2's indirectWorkerSetID just got updated
      {{"w1", wid1}, {"w2", wid123}, {"w3", wid123}},
      history,  // Unchanged
      {{"w1", false}, {"w2", false}, {"w3", false}}  // None can get healthy
    );
  }

  ///
  /// Add w4. Ensure w4's workerSetID becomes {w1, w2, w3, w4}.
  /// Propagate / prune, observe desired outcome, incl:
  ///  * no indirect versions are updated (nor denorm)
  ///

  auto wid1234 = workerSetID(r, 4, hash1234);
  history.insert({4, {folly::none, {w4.shard}}});
  addAndUpdateWorker(
    &r,
    w4,
    wid1234,
    4,
    // Denormalized versions of indirectWorkerSetID change as expected.
    {{1, "w1"}, {3, "w2"}, {3, "w3"}},
    {{1, "w1"}, {3, "w2"}, {3, "w3"}, {4, "w4"}},
    // Indirect WorkerSetIDs are as before.
    {{"w1", wid1}, {"w2", wid123}, {"w3", wid123}},
    history,
    // Almost all the conditions for the workers to be allowed to be healthy
    // are met, but we're still blocked on them failing to indirectly
    // require each other.
    {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
  );

  ///
  /// Ensure w2's workerSetID becomes {w1, w2, w3, w4}
  ///  * w2's indirect version **is** immediately updated (and its denorm)
  /// Propagate / prune, observe desired outcome, incl:
  ///  * w3's indirect version **is** updated (and its denorm)

  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    EXPECT_EQ(wid1234, r.processHeartbeat(&update, w2, wid1234)->workerSetID);
  }
  EXPECT_EQ(wid1234, r.getWorker("w2")->workerSetID());

  // Before propagation/pruning: w2's indirectWorkerSetID just got bumped by
  // the heartbeat.
  updateWorkers(
    &r,
    {{1, "w1"}, {4, "w2"}, {3, "w3"}, {4, "w4"}},  // Bump w2's version
    // w2's indirectWorkerSetID just got updated
    {{"w1", wid1}, {"w2", wid1234}, {"w3", wid123}, {"w4", wid1234}},
    history,
    // w1 doesn't know about any worker but itself, which blocks everyone.
    {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
  );
  // After propagation/pruning: Label propagation advances w3 to match.
  // There is nothing to prune, since w1 is still alive.
  for (int i = 0; i < 2; ++i) {  // Ensure the second pass is a no-op
    updateWorkers(
      &r,
      {{1, "w1"}, {4, "w2"}, {4, "w3"}, {4, "w4"}},  // Bump w3's version
      // w3's indirectWorkerSetID just got updated
      {{"w1", wid1}, {"w2", wid1234}, {"w3", wid1234}, {"w4", wid1234}},
      history,
      // w1 doesn't know about any worker but itself, which blocks everyone.
      {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
    );
  }

  ///
  /// Lose w1 to test history pruning.
  ///

  auto wid234 = workerSetID(r, 5, hash234);
  EXPECT_EQ(RemoteWorkerState::State::HEALTHY, r.getWorker("w1")->getState());
  int64_t t = 1;
  for (auto& p : std::vector<std::pair<RemoteWorkerState::State, int64_t>>{
    std::make_pair(  // Make w1 unhealthy
      RemoteWorkerState::State::UNHEALTHY,
      1 + FLAGS_healthcheck_period + FLAGS_healthcheck_grace_period
        + FLAGS_worker_check_interval
    ),
    std::make_pair(  // Lose w1
      RemoteWorkerState::State::MUST_DIE,
      1 + FLAGS_lose_unhealthy_worker_after
    ),
  }) {
    t += p.second;
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, t);
    // Since w1 gets lost, should not matter that it now requires w2 as well.
    EXPECT_EQ(
      p.first == RemoteWorkerState::State::UNHEALTHY ? wid1234 : wid234,
      r.processHeartbeat(&update, w1, wid12)->workerSetID
    );
    EXPECT_EQ(wid12, r.getWorker("w1")->workerSetID());
    EXPECT_EQ(p.first, r.getWorker("w1")->getState());
  }

  checkWorkerSetIDs(  // w3's workerSetID was never updated
    r, {{"w1", wid12}, {"w2", wid1234}, {"w3", wid123}, {"w4", wid1234}}
  );

  // updateState hasn't run yet, so the only change is the loss of w1.
  history.insert({5, {w1.shard, std::unordered_set<std::string>{}}});
  updateWorkers(
    &r,
    {{4, "w2"}, {4, "w3"}, {4, "w4"}},
    // w1 still has an indirectWorkerSetID, even though it's dead
    {{"w1", wid12}, {"w2", wid1234}, {"w3", wid1234}, {"w4", wid1234}},
    history,
    // w2-w4's workerSetID is out-of-date: wid234 & wid1234.
    {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
  );

  // Now that updateState ran once, we should have pruned history versions 1-3
  history.erase(1);
  history.erase(2);
  history.erase(3);
  history.insert(  // History got pruned: w3 is keeping version 3 alive.
    {3, {folly::none, {w1.shard, w2.shard, w3.shard}}}
  );
  for (int i = 0; i < 2; ++i) {  // Ensure the second pass is a no-op
    updateWorkers(
      &r,
      {{4, "w2"}, {4, "w3"}, {4, "w4"}},
      // w1 still has an indirectWorkerSetID, even though it's dead
      {{"w1", wid12}, {"w2", wid1234}, {"w3", wid1234}, {"w4", wid1234}},
      history,
      // w2-w4's workerSetID is still out-of-date: wid234 & wid1234.
      {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
    );
  }

  ///
  /// Now, let w3 become healthy, in two steps:
  ///  1) Update w3's workerSetID via a heartbeat
  ///  2) updateState so that it propagates to the others.
  ///

  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    EXPECT_EQ(wid234, r.processHeartbeat(&update, w3, wid234)->workerSetID);
  }
  EXPECT_EQ(wid234, r.getWorker("w3")->workerSetID());
  // w3's state changes even before the propagation.
  EXPECT_FALSE(r.consensusPermitsBecomingHealthyForUnitTest("w1"));
  EXPECT_FALSE(r.consensusPermitsBecomingHealthyForUnitTest("w2"));
  EXPECT_TRUE(r.consensusPermitsBecomingHealthyForUnitTest("w3"));
  EXPECT_FALSE(r.consensusPermitsBecomingHealthyForUnitTest("w4"));

  checkWorkerSetIDs(
    r, {{"w1", wid12}, {"w2", wid1234}, {"w3", wid234}, {"w4", wid1234}}
  );

  // w3 no longer keeping v3 alive, but history is not yet pruned.
  updateWorkers(
    &r,
    {{4, "w2"}, {5, "w3"}, {4, "w4"}},  // w2 & w4 have not yet propagated
    {{"w1", wid12}, {"w2", wid1234}, {"w3", wid234}, {"w4", wid1234}},
    history,
    // w2 and w4's workerSetID is still old, so they are blocked
    {{"w1", false}, {"w2", false}, {"w3", true}, {"w4", false}}
  );
  history.erase(3);
  history.erase(4);
  history.insert(  // History got pruned: w2 and w4 are keeping v4 alive.
    {4, {folly::none, {w1.shard, w2.shard, w3.shard, w4.shard}}}
  );
  updateWorkers(
    &r,
    {{5, "w2"}, {5, "w3"}, {5, "w4"}},  // w2 & w4 have now been propagated
    {{"w1", wid12}, {"w2", wid234}, {"w3", wid234}, {"w4", wid234}},
    history,
    {{"w1", false}, {"w2", false}, {"w3", true}, {"w4", false}}
  );

  ///
  /// Reincarnate w1, which tests the "worker replace" code path, which
  /// has a number of subtle differences. This test caught several bugs.
  ///

  auto w1a = makeWorker("w1");
  w1a.id.startTime = w1.id.startTime + 1;  // This is a **newer** worker.
  CHECK_GT(w1a.id.startTime, w1.id.startTime);  // Fail loudly if we overflow.
  cpp2::WorkerSetHash hash1a234 = hash234;
  addWorkerIDToHash(&hash1a234, w1a.id);
  auto wid1a234 = workerSetID(r, 6, hash1a234);
  history.insert({6, {folly::none, {w1a.shard}}});
  addAndUpdateWorker(
    &r,
    w1a,
    wid1a234,
    4,
    {{5, "w2"}, {5, "w3"}, {5, "w4"}},
    {{5, "w2"}, {5, "w3"}, {5, "w4"}, {6, "w1"}},
    {{"w2", wid234}, {"w3", wid234}, {"w4", wid234}, {"w1", wid1a234}},
    history,
    // w2-w4's workerSetIDs are out of date, while w1a isn't known by anyone
    {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
  );

  ///
  /// Lose w4 to make a new version, so versions 5-7 exist.
  ///

  cpp2::WorkerSetHash hash1a23 = hash1a234;
  removeWorkerIDFromHash(&hash1a23, w4.id);
  auto wid1a23 = workerSetID(r, 7, hash1a23);
  EXPECT_EQ(
    RemoteWorkerState::State::UNHEALTHY, r.getWorker("w4")->getState()
  );
  {
    RemoteWorkerUpdate update(
      RemoteWorkerUpdate::UNIT_TEST_TIME, 2 + FLAGS_lose_unhealthy_worker_after
    );
    // Catch w4 up to the latest workerSetID, since we do propagate through
    // MUST_DIE workers, and this will have the effect of catching w2 & w3
    // to version 6 after a propagation.
    EXPECT_EQ(wid1a23, r.processHeartbeat(&update, w4, wid1a234)->workerSetID);
  }
  EXPECT_EQ(wid1a234, r.getWorker("w4")->workerSetID());
  EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, r.getWorker("w4")->getState());

  history.insert({7, {w4.shard, std::unordered_set<std::string>{}}});
  updateWorkers(
    &r,
    {{5, "w2"}, {5, "w3"}, {6, "w1"}},
    {{"w1", wid1a234}, {"w2", wid234}, {"w3", wid234}, {"w4", wid1a234}},
    history,
    // All workerSetIDs are out of date
    {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
  );
  // A propagation through the dead w4 updates w2 & w3.
  updateWorkers(
    &r,
    {{6, "w2"}, {6, "w3"}, {6, "w1"}},
    {{"w1", wid1a234}, {"w2", wid1a234}, {"w3", wid1a234}, {"w4", wid1a234}},
    history,
    // All workerSetIDs are out of date -- nobody knows of w4's departure.
    {{"w1", false}, {"w2", false}, {"w3", false}, {"w4", false}}
  );

  // At this point, our active versions are 4-7, but only 6 & 7 are
  // referenced.  Yet, the above propagation passes worked fine.
  //
  // Future: the truly pedantic should add a test with a point in history,
  // where all prior workers are deleted, and then new ones are added.  This
  // would exercise the case of `vss` being empty in the middle of the
  // propagation loop, but I'm pretty sure it's correct already.
  checkWorkerSetIDs(
    r, {{"w1", wid1a234}, {"w2", wid1234}, {"w3", wid234}, {"w4", wid1a234}}
  );

  ///
  /// Bump w1a's workerSetID to v7, and watch it propagate.
  ///

  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    EXPECT_EQ(wid1a23, r.processHeartbeat(&update, w1a, wid1a23)->workerSetID);
  }
  EXPECT_EQ(wid1a23, r.getWorker("w1")->workerSetID());
  // Before propagation, nothing changed except w1a's indirect version.
  updateWorkers(
    &r,
    {{6, "w2"}, {6, "w3"}, {7, "w1"}},
    {{"w1", wid1a23}, {"w2", wid1a234}, {"w3", wid1a234}, {"w4", wid1a234}},
    history,
    // w1 and w4's workerSetIDs are the two up-to-date ones, but w4 is dead.
    // (Remember this check runs post-propagation.)
    {{"w1", true}, {"w2", false}, {"w3", false}, {"w4", false}}
  );

  // After propagation, all workers are up to
  updateWorkers(
    &r,
    {{7, "w2"}, {7, "w3"}, {7, "w1"}},
    // w4 doesn't get updated since it's dead. Yes, it's a little weird that
    // we propagate *through* dead workers, but do not update them, but the
    // implementation is clean, and there are no implementations for
    // correctness, as argued in README.worker_set_consensus.
    {{"w1", wid1a23}, {"w2", wid1a23}, {"w3", wid1a23}, {"w4", wid1a234}},
    history,
    //
    {{"w1", true}, {"w2", false}, {"w3", false}, {"w4", false}}
  );
}
