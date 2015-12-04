/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
  EXPECT_TRUE(res.hasValue());
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
  auto res = r.processHeartbeat(&update, w1, cpp2::WorkerSetID());
  EXPECT_EQ(static_cast<int>(RemoteWorkerState::State::NEW), res->workerState);
  addWorkerIDToHash(&wh, w1.id);
  EXPECT_EQ(workerSetID(r, 1, wh), res->workerSetID);
  EXPECT_EQ(IDSet{w1.id}, dredgeHostPool(r, "host1"));

  res = r.processHeartbeat(&update, w2, cpp2::WorkerSetID());
  EXPECT_EQ(static_cast<int>(RemoteWorkerState::State::NEW), res->workerState);
  addWorkerIDToHash(&wh, w2.id);
  EXPECT_EQ(workerSetID(r, 2, wh), res->workerSetID);
  EXPECT_EQ(IDSet({w1.id, w2.id}), dredgeHostPool(r, "host1"));

  res = r.processHeartbeat(&update, w3, cpp2::WorkerSetID());
  EXPECT_EQ(static_cast<int>(RemoteWorkerState::State::NEW), res->workerState);
  addWorkerIDToHash(&wh, w3.id);
  EXPECT_EQ(workerSetID(r, 3, wh), res->workerSetID);
  EXPECT_EQ(IDSet({w1.id, w2.id}), dredgeHostPool(r, "host1"));
  EXPECT_EQ(IDSet({w3.id}), dredgeHostPool(r, "host2"));

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

  // Move a worker from one host to another
  auto w1new = makeWorker("w1");
  w1new.machineLock.hostname = "host2";
  w1new.machineLock.port = 789;
  ASSERT_NE(w1.id, w1new.id);
  {
    RemoteWorkerUpdate update2(
      RemoteWorkerUpdate::UNIT_TEST_TIME, FLAGS_lose_unhealthy_worker_after + 1
    );

    res = r.processHeartbeat(&update2, w1new, cpp2::WorkerSetID());
    EXPECT_EQ(
      static_cast<int>(RemoteWorkerState::State::NEW), res->workerState
    );
    removeWorkerIDFromHash(&wh, w1.id);
    addWorkerIDToHash(&wh, w1new.id);
    auto wsid = workerSetID(r, 5, wh);  // 1 deletion, 1 addition == 2 versions
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

struct TestRemoteWorkersInitialWait : public ::testing::Test {
  TestRemoteWorkersInitialWait() {
    // Known values for more predictable tests
    FLAGS_CAUTION_startup_wait_for_workers = -1;
    FLAGS_worker_check_interval = 1;
    FLAGS_healthcheck_period = 3;
    FLAGS_healthcheck_grace_period = 9;
    FLAGS_lose_unhealthy_worker_after = 27;
  }
};

const time_t kSafeInitialWait = 27 + 9 + 3 + 1 + 1;

// Check the timeouts with no workers.
TEST_F(TestRemoteWorkersInitialWait, NoWorkers) {
  RemoteWorkers r(0, randInstanceID());
  // The last 3 shows that once the wait is over, it can't be turned back on.
  for (time_t t : std::vector<time_t>{0, 1, 5, 39, 40, kSafeInitialWait, 3}) {
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

// Add the worker, and get it out of NEW state, while monitoring the
// consensus variables-related variables.  Uses the timestamp '1'.
void addWorker(
    RemoteWorkers* r,
    const cpp2::BistroWorker& w,
    const cpp2::WorkerSetID& initial_id,
    const std::multiset<cpp2::WorkerSetID>& expected_initial_ids,
    const cpp2::WorkerSetID& expected_non_mustdie_id) {

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
      r->updateState(&update);
      EXPECT_PCRE_MATCH(".*Waiting for all.*", update.initialWaitMessage());
    }
    auto rw = r->getWorker(w.shard);
    EXPECT_EQ(RemoteWorkerState::State::NEW, rw->getState());
    EXPECT_EQ(initial_id, rw->initialWorkerSetID());
    EXPECT_FALSE(rw->workerSetID().hasValue());
    // This is set the first time the WorkerSetID changes from the initial.
    EXPECT_FALSE(rw->firstAssociatedWorkerSetID().hasValue());
    EXPECT_EQ(expected_non_mustdie_id, r->nonMustDieWorkerSetID());
    EXPECT_EQ(expected_initial_ids, r->initialWorkerSetIDs());
  }

  // Once the worker fetches running tasks, it's ready to become healthy.
  r->initializeRunningTasks(w, {});
  EXPECT_EQ(
    RemoteWorkerState::State::UNHEALTHY, r->getWorker(w.shard)->getState()
  );
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    r->updateState(&update);
    EXPECT_PCRE_MATCH(".*Waiting for all.*", update.initialWaitMessage());
  }
  EXPECT_EQ(
    RemoteWorkerState::State::HEALTHY, r->getWorker(w.shard)->getState()
  );
}

// A "script" test wherein 2 workers leave initial wait early by achieving
// consensus (with an impostor worker joining in the middle).
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

  addWorker(&r, w2, id12, {id12}, workerSetID(r, 1, id2.hash));
  addWorker(&r, w3, id3, {id12, id3}, workerSetID(r, 2, id23.hash));
  addWorker(&r, w1, id12, {id12, id12, id3}, workerSetID(r, 3, id123.hash));

  // Now, lose w3, and thus achieve consensus. Use heartbeats to this
  // effect in order to avoid messing with the other workers.
  int64_t t = 1;
  for (auto& p : std::vector<std::pair<RemoteWorkerState::State, int64_t>>{
    std::make_pair(
      RemoteWorkerState::State::UNHEALTHY,
      1 + FLAGS_healthcheck_period + FLAGS_healthcheck_grace_period
        + FLAGS_worker_check_interval
    ),
    std::make_pair(
      RemoteWorkerState::State::MUST_DIE,
      1 + FLAGS_lose_unhealthy_worker_after
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
      ASSERT_FALSE(maybe_id.hasValue());
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
  addWorker(&r, w, id, {id}, workerSetID(r, 1, h));

  // There is no consensus, so we remain in initial wait.
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 1);
    r.updateState(&update);
    EXPECT_NE("", update.initialWaitMessage());
  }
  EXPECT_NO_PCRE_MATCH(glogErrorPattern(), stderr.readIncremental());
}
