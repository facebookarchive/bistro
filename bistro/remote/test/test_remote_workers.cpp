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

#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkers.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"

using namespace facebook::bistro;
using namespace folly::test;

using IDSet = std::set<cpp2::BistroInstanceID>;

DECLARE_int32(CAUTION_startup_wait_for_workers);

cpp2::BistroWorker makeWorker(std::string shard) {
  cpp2::BistroWorker worker;
  worker.shard = shard;
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  worker.id.startTime = folly::Random::rand64();
  worker.id.rand = folly::Random::rand64();
  return worker;
}

TEST(TestRemoteWorkers, ProtocolMismatch) {
  auto worker = makeWorker("w");
  RemoteWorkerUpdate update;
  RemoteWorkers r(0);

  // Mismatched version
  worker.protocolVersion = -1;
  EXPECT_THROW(r.processHeartbeat(&update, worker), std::runtime_error);
  EXPECT_TRUE(r.workerPool().begin() == r.workerPool().end());

  // Matched version
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  auto res = r.processHeartbeat(&update, worker);
  EXPECT_TRUE(res.hasValue());
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
  RemoteWorkers r(0);

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

  // getNextWorkerByHost
  EXPECT_EQ(
    static_cast<int>(RemoteWorkerState::State::NEW),
    r.processHeartbeat(&update, w1)->workerState
  );
  EXPECT_EQ(IDSet{w1.id}, dredgeHostPool(r, "host1"));

  EXPECT_EQ(
    static_cast<int>(RemoteWorkerState::State::NEW),
    r.processHeartbeat(&update, w2)->workerState
  );
  EXPECT_EQ(IDSet({w1.id, w2.id}), dredgeHostPool(r, "host1"));

  EXPECT_EQ(
    static_cast<int>(RemoteWorkerState::State::NEW),
    r.processHeartbeat(&update, w3)->workerState
  );
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
    EXPECT_EQ(
      static_cast<int>(RemoteWorkerState::State::NEW),
      r.processHeartbeat(&update2, w1new)->workerState
    );
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
  RemoteWorkers r(0);
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

// Check timeouts with one new worker. Also check that we wait to get its
// running tasks, since it's very risky to start new tasks when you have a
// known & somewhat responsive worker that may have some unknown tasks.
TEST_F(TestRemoteWorkersInitialWait, OneWorkerWithRunningTasks) {
  // I want to play the game of "the worker remains in NEW status when the
  // initial wait expires".  A safe initial wait is set up so that such
  // workers will commit suicide first, so use an unsafe one.
  const int kInitialWait = 10;
  FLAGS_CAUTION_startup_wait_for_workers = kInitialWait;
  RemoteWorkers r(0);
  auto w = makeWorker("w");
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, 0);
    r.processHeartbeat(&update, w);
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
