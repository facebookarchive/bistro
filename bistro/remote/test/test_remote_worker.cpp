/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/remote/WorkerSetID.h"
#include "bistro/bistro/statuses/TaskStatus.h"

// Future: could add a direct test for StatusChangeCob. For now, it's
// adequately tested (indirectly) by test_remote_workers.cpp.

using namespace facebook::bistro;
using namespace folly::test;

using apache::thrift::debugString;

DECLARE_bool(allow_bump_unhealthy_worker);
DECLARE_int32(unsure_if_running_check_initial_period);

void expectUpdateEq(const RemoteWorkerUpdate& a, const RemoteWorkerUpdate& b) {
  EXPECT_EQ(a.curTime(), b.curTime());
  EXPECT_TRUE(a.workersToHealthcheck() == b.workersToHealthcheck());
  EXPECT_TRUE(a.suicideWorkers() == b.suicideWorkers());
  EXPECT_TRUE(a.newWorkers() == b.newWorkers());
  EXPECT_TRUE(a.lostRunningTasks() == b.lostRunningTasks());
  EXPECT_TRUE(
    a.unsureIfRunningTasksToCheck() == b.unsureIfRunningTasksToCheck()
  );
}

// Copy-pasta'd from worker/test/utils.cpp
void printString(folly::StringPiece s) {
  if (!s.empty()) {
    std::cout << "stderr: " << s << std::flush;
  }
}

// Use a non-default scheduler ID in the initial WorkerSetID since
// RemoteWorker::updateCurrentWorker expects the initial set ID to come from
// a different scheduler than the current one (with caveats).  This lets
// most tests use the default 0/0 schedulerID for less code.
cpp2::WorkerSetID initialSetID() {
  cpp2::WorkerSetID wid;
  wid.schedulerID.startTime = 3141592654;
  return wid;
}

RemoteWorker initializeWorker(
    int64_t test_time,
    const std::vector<cpp2::RunningTask>& running_tasks) {

  cpp2::BistroWorker bw;
  bw.protocolVersion = cpp2::common_constants::kProtocolVersion();
  bw.id.startTime = 2718281828;  // Make it easier to read WorkerSetIDs

  // RemoteWorkers creates a worker whenever it sees a new shard ID.
  RemoteWorker worker(test_time, bw, initialSetID(), cpp2::BistroInstanceID());
  EXPECT_EQ(initialSetID(), worker.initialWorkerSetID());
  EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());

  // Check that updateState() requests a health-check & new tasks
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ false);

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.addNewWorker(bw);
    expected.healthcheckWorker(bw);
    expectUpdateEq(expected, update);
  }

  worker.initializeRunningTasks(running_tasks);
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
  EXPECT_FALSE(worker.hasBeenHealthy());
  EXPECT_FALSE(worker.firstAssociatedWorkerSetID().has_value());
  return worker;
}

void successfulHealthcheck(int64_t test_time, RemoteWorker* worker) {
  // This does not update timeLastHealthcheckSent_, so RemoteWorker may
  // request another healthcheck despite this "success" being registered.
  // To predictably **not** be requesting a redundant healthcheck, you
  // should run this **after** a heartbeat with the current test_time.
  cpp2::RunningTask rt;
  rt.job = kHealthcheckTaskJob;
  rt.invocationID.startTime = test_time;
  EXPECT_FALSE(worker->recordNonRunningTaskStatus(
    rt, TaskStatus::done(), worker->getBistroWorker().id
  ));
}

// If the update is mutated after construction, we throw.
struct EnforcedNoOpUpdate {
  explicit EnforcedNoOpUpdate(int64_t test_time)
    : testTime_(test_time),
      update_(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time) {}

  ~EnforcedNoOpUpdate() {
    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, testTime_);
    // Surprisingly, failing test assertions in a destructor works fine.
    expectUpdateEq(expected, update_);
  }

  int64_t testTime_;
  RemoteWorkerUpdate update_;
};

cpp2::WorkerSetID sendFirstHeartbeat(
    RemoteWorkerUpdate* update,
    RemoteWorker* worker) {
  // This is only called on new workers, so the default version of 0 is ok.
  cpp2::WorkerSetID wid;

  // consensus_permits_becoming_healthy is needed to get healthy for the
  // first time.
  EXPECT_TRUE(
      worker->processHeartbeat(update, worker->getBistroWorker(), wid, false)
          .has_value());
  EXPECT_EQ(wid, worker->firstAssociatedWorkerSetID());
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker->getState());
  EXPECT_FALSE(worker->hasBeenHealthy());

  return wid;
}

void makeWorkerHealthy(int64_t test_time, RemoteWorker* worker) {
  successfulHealthcheck(test_time, worker);
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker->getState());

  // A healthcheck isn't enough, send a fresh heartbeat.
  EnforcedNoOpUpdate update(test_time);

  const auto first_wid = sendFirstHeartbeat(&update.update_, worker);
  auto wid = first_wid;  // We'll evolve this copy

  // Just as above, but consensus_permits_becoming_healthy is true.
  // Drive-by test: make sure the worker set ID is echoed correctly.
  EXPECT_NE(cpp2::BistroInstanceID(), worker->getBistroWorker().id);
  ASSERT_TRUE(worker->workerSetID().has_value());
  EXPECT_EQ(wid, worker->workerSetID())
    << debugString(wid) << " != " << debugString(*worker->workerSetID());
  addWorkerIDToHash(&wid.hash, worker->getBistroWorker().id);
  ++wid.version;
  EXPECT_TRUE(worker
                  ->processHeartbeat(
                      &update.update_, worker->getBistroWorker(), wid, true)
                  .has_value());
  EXPECT_EQ(first_wid, worker->firstAssociatedWorkerSetID());
  EXPECT_EQ(wid, worker->workerSetID());
  EXPECT_TRUE(worker->hasBeenHealthy());
  EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker->getState());

  // consensus_permits_becoming_healthy isn't needed once we've been healthy
  EXPECT_TRUE(worker
                  ->processHeartbeat(
                      &update.update_, worker->getBistroWorker(), wid, false)
                  .has_value());
  EXPECT_EQ(first_wid, worker->firstAssociatedWorkerSetID());
  EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker->getState());
}

TEST(TestRemoteWorker, HandleNormal) {
  CaptureFD stderr(2, printString);
  int64_t test_time = 0;

  auto worker = initializeWorker(test_time, {});
  makeWorkerHealthy(test_time, &worker);

  // Make the worker unhealthy by missing a heartbeat for long enough.
  // The default timeout for healthchecks is larger, so it remains okay.
  {
    // Add 1 because the code uses max(1, heartbeat period).
    test_time += FLAGS_heartbeat_grace_period
       + worker.getBistroWorker().heartbeatPeriodSec + 1;
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ true);
    EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
    EXPECT_TRUE(worker.hasBeenHealthy());
    EXPECT_PCRE_MATCH(glogWarningPattern(), stderr.readIncremental());

    // The default heartbeat grace period advances the time past the default
    // healthcheck period (but the previous healthcheck is still valid).
    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.healthcheckWorker(worker.getBistroWorker());
    expectUpdateEq(expected, update);
  }

  // Check that the next heartbeat updates the worker's metadata.
  auto bw = worker.getBistroWorker();
  ++bw.heartbeatPeriodSec;
  EXPECT_NE(bw, worker.getBistroWorker());
  // Another heartbeat, and it'll be healthy again.
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    cpp2::WorkerSetID wid;  // Must increment version to avoid error logs.
    wid.version = worker.workerSetID()->version + 1;
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update, bw, wid, /*permit_becoming_healthy =*/false)
                    .has_value());
    EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker.getState());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expectUpdateEq(expected, update);
  }
  EXPECT_EQ(bw, worker.getBistroWorker());  // Got the new metadata

  EXPECT_TRUE(worker.hasBeenHealthy());
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
}

TEST(TestRemoteWorker, DoNotDieDueToLackOfConsensus) {
  // In both iterations of the loop, the worker is unhealthy for the same
  // time period.  The only difference is that in one case, it is solely
  // unhealthy because of the lack of consensus -- and thus is not lost.  In
  // the other case, it has other issues, and does get lost.
  for (int do_not_lose = 0; do_not_lose <= 1; ++do_not_lose) {
    CaptureFD stderr(2, printString);
    int64_t test_time = 0;

    // Give our worker a running task (exercises running task tracking)
    cpp2::RunningTask rt;
    rt.job = "foobar";

    auto worker = initializeWorker(
      // The worker has been unhealthy long enough that it's about to be lost.
      test_time - FLAGS_lose_unhealthy_worker_after - 1,
      {rt}
    );
    const auto bw = worker.getBistroWorker();

    // A heartbeat and a health-check will get the worker to the point of
    // being unhealthy **only** because of the lack of consensus.  Use
    // `test_time - 1` since it is before the "lose unhealthy after" time.
    if (do_not_lose) {
      RemoteWorkerUpdate u(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time - 1);
      auto wid = sendFirstHeartbeat(&u, &worker);

      // The heartbeat leads us to request a health-check.
      RemoteWorkerUpdate e(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time - 1);
      e.healthcheckWorker(bw);
      expectUpdateEq(e, u);

      // And now we fake a "healthcheck successful" response.
      EnforcedNoOpUpdate u2(test_time - 1);
      EXPECT_TRUE(
          worker.processHeartbeat(&u2.update_, bw, wid, false).has_value());
      successfulHealthcheck(test_time - 1, &worker);
      EXPECT_TRUE(
          worker.processHeartbeat(&u2.update_, bw, wid, false).has_value());
    }
    EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());

    // If lack of consensus is the only problem, the worker stays unhealthy.
    // Otherwise, it is lost.
    {
      RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
      worker.updateState(&update, /*permit_becoming_healthy =*/ false);

      RemoteWorkerUpdate expected(
        RemoteWorkerUpdate::UNIT_TEST_TIME, test_time
      );
      if (do_not_lose) {
        EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
      } else {
        EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, worker.getState());
        expected.requestSuicide(bw, "Current worker just became lost");
        expected.loseRunningTask(std::make_pair(rt.job, rt.node), rt);
      }
      expectUpdateEq(expected, update);
    }

  }
}

TEST(TestRemoteWorker, HandleMustDieAndLostTasks) {
  CaptureFD stderr(2, printString);
  int64_t test_time = 0;

  cpp2::BistroWorker bw;
  bw.protocolVersion = cpp2::common_constants::kProtocolVersion();
  // The worker has been unhealthy long enough that it's about to be lost.
  RemoteWorker worker(
    test_time - FLAGS_lose_unhealthy_worker_after - 1,
    bw,
    initialSetID(),
    cpp2::BistroInstanceID()
  );
  EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());

  // Give it a running task (exercises running task tracking)
  cpp2::RunningTask rt;
  rt.job = "foobar";
  worker.initializeRunningTasks({rt});
  // DO: Add some way to inspect this and confirm it was cleared after
  // the MUST_DIE state change?
  worker.addUnsureIfRunningTask(rt);
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
  EXPECT_FALSE(worker.hasBeenHealthy());

  // Lose the worker
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ true);
    EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, worker.getState());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw, "Current worker just became lost");
    expected.loseRunningTask(std::make_pair(rt.job, rt.node), rt);
    expectUpdateEq(expected, update);
  }

  // Check that it stays lost, making no updates
  {
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ true);
    EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, worker.getState());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expectUpdateEq(expected, update);
  }

  EXPECT_FALSE(worker.hasBeenHealthy());
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
}

// Covers all the ways for processHeartbeat to replace the current worker.
TEST(TestRemoteWorker, WorkerReplacement) {
  CaptureFD stderr(2, printString);
  int64_t test_time = 0;

  // Make a worker with a running task
  cpp2::RunningTask rt;
  rt.job = "foobar";
  auto worker = initializeWorker(test_time, {rt});
  makeWorkerHealthy(test_time, &worker);

  ///
  /// Start with the 3 ways of replacing a worker with the same machineLock.
  /// (same host listening on the same port, so the scheduler is sure that
  /// the old worker is dead)
  ///

  // Replace it via a heartbeat from a new worker with the same machineLock
  {
    auto bw_new = worker.getBistroWorker();
    ++bw_new.id.startTime;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update,
                        bw_new,
                        initialSetID(),
                        /*permit_becoming_healthy =*/true)
                    .has_value());
    EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
    EXPECT_EQ(bw_new, worker.getBistroWorker());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.loseRunningTask(std::make_pair(rt.job, rt.node), rt);
    expected.addNewWorker(bw_new);
    expected.healthcheckWorker(bw_new);
    expectUpdateEq(expected, update);  // No suicide requests
  }
  worker.initializeRunningTasks({});
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
  makeWorkerHealthy(test_time, &worker);

  // Since this heartbeat has the same machineLock and a smaller timestamp,
  // it will be ignored (with a warning).
  {
    auto bw_old = worker.getBistroWorker();
    auto bw_new = bw_old;
    --bw_new.id.startTime;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
    EXPECT_FALSE(worker
                     .processHeartbeat(
                         &update,
                         bw_new,
                         initialSetID(),
                         /*permit_becoming_healthy =*/false)
                     .has_value());
    EXPECT_PCRE_MATCH(glogWarningPattern(), stderr.readIncremental());
    EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker.getState());
    EXPECT_EQ(bw_old, worker.getBistroWorker());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expectUpdateEq(expected, update);
  }

  // This "bad" worker has the same machineLock & startTime, but a different
  // rand, meaning that the scheduler cannot reliably distinguish whether
  // it's newer or older than the one it currently has, and logs an error.
  {
    auto bw_new = worker.getBistroWorker();
    ++bw_new.id.rand;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update,
                        bw_new,
                        initialSetID(),
                        /*permit_becoming_healthy =*/true)
                    .has_value());
    EXPECT_PCRE_MATCH("E[^\n]* different rands:.*", stderr.readIncremental());
    EXPECT_FALSE(worker.hasBeenHealthy());
    EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
    EXPECT_EQ(bw_new, worker.getBistroWorker());  // The bad one wins

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.addNewWorker(bw_new);
    expected.healthcheckWorker(bw_new);
    expectUpdateEq(expected, update);  // No suicide requests
  }
  worker.initializeRunningTasks({});
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
  makeWorkerHealthy(test_time, &worker);

  ///
  /// Now a worker with a different machineLock tries to replace a current
  /// worker in all possible statuses: HEALTHY, MUST_DIE, UNHEALTHY, NEW.
  /// It can never bump a HEALTHY one, and can only bump UNHEALTHY and NEW
  /// when FLAGS_allow_bump_unhealthy_worker is set.
  ///

  // Since the current worker is healthy, the replacement is told to suicide
  {
    auto bw_old = worker.getBistroWorker();
    auto bw_new = bw_old;
    ++bw_new.machineLock.port;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_FALSE(worker
                     .processHeartbeat(
                         &update,
                         bw_new,
                         initialSetID(),
                         /*permit_becoming_healthy =*/false)
                     .has_value());
    EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker.getState());
    EXPECT_EQ(bw_old, worker.getBistroWorker());  // The old one wins

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_new, "The old worker is still okay");
    expectUpdateEq(expected, update);
  }

  // Make the worker unhealthy by missing a heartbeat for long enough.  The
  // default timeout for healthchecks is larger, so that part remains okay.
  {
    // Add 1 because the code uses max(1, heartbeat period).
    test_time += FLAGS_heartbeat_grace_period
       + worker.getBistroWorker().heartbeatPeriodSec + 1;
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ true);
    EXPECT_TRUE(worker.hasBeenHealthy());
    EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
    EXPECT_PCRE_MATCH(glogWarningPattern(), stderr.readIncremental());

    // The default heartbeat grace period advances the time past the default
    // healthcheck period (but the previous healthcheck is still valid).
    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.healthcheckWorker(worker.getBistroWorker());
    expectUpdateEq(expected, update);
  }

  // Cannot replace an unhealthy worker unless this flag is set
  FLAGS_allow_bump_unhealthy_worker = 0;
  {
    auto bw_old = worker.getBistroWorker();
    auto bw_new = bw_old;
    ++bw_new.machineLock.port;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_FALSE(worker
                     .processHeartbeat(
                         &update,
                         bw_new,
                         initialSetID(),
                         /*permit_becoming_healthy =*/true)
                     .has_value());
    EXPECT_TRUE(worker.hasBeenHealthy());
    EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, worker.getState());
    EXPECT_EQ(bw_old, worker.getBistroWorker());  // The old one wins

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_new, "The old worker is still okay");
    expectUpdateEq(expected, update);
  }

  // Flip the flag and the worker is replaced
  FLAGS_allow_bump_unhealthy_worker = 1;
  {
    auto bw_old = worker.getBistroWorker();
    auto bw_new = bw_old;
    ++bw_new.machineLock.port;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update,
                        bw_new,
                        initialSetID(),
                        /*permit_becoming_healthy =*/true)
                    .has_value());
    EXPECT_FALSE(worker.hasBeenHealthy());
    EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
    EXPECT_EQ(bw_new, worker.getBistroWorker());  // The new one wins

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_old, "Current unhealthy worker was replaced");
    expected.addNewWorker(bw_new);
    expected.healthcheckWorker(bw_new);
    expectUpdateEq(expected, update);
  }

  // The flag is set, our worker is NEW (not healthy), and so can be replaced
  {
    auto bw_old = worker.getBistroWorker();
    auto bw_new = bw_old;
    ++bw_new.machineLock.port;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update,
                        bw_new,
                        initialSetID(),
                        /*permit_becoming_healthy =*/true)
                    .has_value());
    EXPECT_FALSE(worker.hasBeenHealthy());
    EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
    EXPECT_EQ(bw_new, worker.getBistroWorker());  // The new one wins

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_old, "Current unhealthy worker was replaced");
    expected.addNewWorker(bw_new);
    expected.healthcheckWorker(bw_new);
    expectUpdateEq(expected, update);
  }

  // Make the current worker MUST_DIE
  test_time += FLAGS_lose_unhealthy_worker_after + 1;
  {
    auto bw_old = worker.getBistroWorker();

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ true);
    EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, worker.getState());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_old, "Current worker just became lost");
    expectUpdateEq(expected, update);
  }

  // A heartbeat from a MUST_DIE worker just re-requests suicide
  {
    auto bw_old = worker.getBistroWorker();

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update,
                        bw_old,
                        cpp2::WorkerSetID(),
                        /*permit_becoming_healthy =*/true)
                    .has_value());
    EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, worker.getState());
    EXPECT_EQ(bw_old, worker.getBistroWorker());

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_old, "Current worker was already lost");
    expectUpdateEq(expected, update);
  }
  EXPECT_FALSE(worker.hasBeenHealthy());

  // Now it can be replaced despite the flag being off
  FLAGS_allow_bump_unhealthy_worker = 0;
  {
    auto bw_old = worker.getBistroWorker();
    auto bw_new = bw_old;
    ++bw_new.machineLock.port;

    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    EXPECT_TRUE(worker
                    .processHeartbeat(
                        &update,
                        bw_new,
                        initialSetID(),
                        /*permit_becoming_healthy =*/true)
                    .has_value());
    EXPECT_FALSE(worker.hasBeenHealthy());
    EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
    EXPECT_EQ(bw_new, worker.getBistroWorker());  // The new one wins

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expected.requestSuicide(bw_old, "Current lost worker was replaced");
    expected.addNewWorker(bw_new);
    expected.healthcheckWorker(bw_new);
    expectUpdateEq(expected, update);
  }

  EXPECT_FALSE(worker.hasBeenHealthy());
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
}

// Covers unsure-if-running task checks, and their exponential backoff.
TEST(TestRemoteWorker, UnsureIfRunning) {
  CaptureFD stderr(2, printString);
  int64_t test_time = 0;

  // Make a worker with a running task
  cpp2::RunningTask rt;
  rt.job = "foobar";
  auto worker = initializeWorker(test_time, {rt});
  makeWorkerHealthy(test_time, &worker);
  const auto& bw = worker.getBistroWorker();
  cpp2::WorkerSetID wid;  // The version can only increase
  wid.version = worker.workerSetID()->version + 1;

  int p = std::max(1, FLAGS_unsure_if_running_check_initial_period);
  for (int i = 0; i < 3; ++i) {  // Run through thrice to test 2 transitions
    // Exercise exponential backoff
    worker.addUnsureIfRunningTask(rt);
    for (const auto& delay_vs_check : std::vector<std::pair<int, bool>>{
      {0, true},
      {(p << 1) - 1, false},
      {1, true},
      {(p << 2) - 1, false},
      {1, true},
      {(p << 3) - 1, false},
      {1, true},
      {(p << 4) - 1, false},
      {1, true},
      {(p << 5) - 1, false},
      {1, true},
      {(p << 6) - 1, false},
      {1, true},
      {(p << 7) - 1, false},
      {1, true},
      {(p << 8) - 1, false},
      {1, true},
      {(p << 8) - 1, false},  // Maximum exponential backoff reached
      {1, true},
    }) {
      test_time += delay_vs_check.first;
      successfulHealthcheck(test_time, &worker);

      RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
      EXPECT_TRUE(worker
                      .processHeartbeat(
                          &update, bw, wid, /*permit_becoming_healthy =*/false)
                      .has_value());
      EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker.getState());

      RemoteWorkerUpdate
        expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
      if (delay_vs_check.second) {
        expected.checkUnsureIfRunningTasks(
          bw, {{std::make_pair(rt.job, rt.node), rt}}
        );
      }
      if (update.workersToHealthcheck().size()) {
        expected.healthcheckWorker(bw);
      }
      expectUpdateEq(expected, update);
    }

    // Between loop iterations, remove the "unsure" task
    if (i == 0) {
      worker.eraseUnsureIfRunningTasks({rt});
    } else if (i == 1) {
      EXPECT_TRUE(worker.recordNonRunningTaskStatus(
        rt, TaskStatus::errorBackoff("hi"), bw.id
      ));
    }

    // Since there are no "unsure" tasks, this resets the backoff count
    RemoteWorkerUpdate update(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    worker.updateState(&update, /*permit_becoming_healthy =*/ false);

    RemoteWorkerUpdate expected(RemoteWorkerUpdate::UNIT_TEST_TIME, test_time);
    expectUpdateEq(expected, update);

    ++test_time;  // So that the first inner iteration triggers a check
  }

  // Note: The lost task test above already checked the interaction of
  // unsure-if-running tasks with loseRunningTasks.

  EXPECT_TRUE(worker.hasBeenHealthy());
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
}

// Exercise recordRunningTaskStatus, recordFailedTask, and
// recordNonRunningTaskStatus.
TEST(TestRemoteWorker, RecordStatuses) {
  CaptureFD stderr(2, printString);
  int64_t test_time = 0;

  // recordNonRunningTaskStatus throws if the worker is in the NEW state
  {
    cpp2::BistroWorker bw;
    bw.protocolVersion = cpp2::common_constants::kProtocolVersion();
    RemoteWorker worker(
      test_time, bw, initialSetID(), cpp2::BistroInstanceID()
    );
    EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
    EXPECT_THROW(
      worker.recordNonRunningTaskStatus(
        cpp2::RunningTask(), TaskStatus::done(), bw.id
      ),
      std::runtime_error
    );
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
  }

  auto worker = initializeWorker(test_time, {});
  makeWorkerHealthy(test_time, &worker);
  const auto& bw = worker.getBistroWorker();

  // An unsuccessful healthcheck prints an error.
  // Note: successfulHealthcheck already tested successful healthchecks
  {
    cpp2::RunningTask rt;
    rt.job = kHealthcheckTaskJob;
    rt.invocationID.startTime = test_time;
    EXPECT_FALSE(worker.recordNonRunningTaskStatus(
      rt, TaskStatus::failed(), bw.id
    ));
    EXPECT_PCRE_MATCH(glogErrorPattern(), stderr.readIncremental());
  }

  cpp2::RunningTask rt;
  rt.job = "foobar";

  // Exercise recordRunningTaskStatus & recordFailedTask
  for (int i = 0; i < 2; ++i) {
    worker.recordRunningTaskStatus(rt, TaskStatus::running());
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
    worker.recordFailedTask(rt, TaskStatus::errorBackoff("d'oh"));
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
  }

  // recordNonRunningTaskStatus throws on bad worker IDs
  auto bad_id = bw.id;
  ++bad_id.startTime;
  EXPECT_THROW(
    worker.recordNonRunningTaskStatus(rt, TaskStatus::done(), bad_id),
    std::runtime_error
  );
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());

  auto overwriteable_status = TaskStatus::errorBackoff("d'oh");
  overwriteable_status.markOverwriteable();

  // Normal behavior for recordNonRunningTaskStatus
  for (const auto& status : std::vector<TaskStatus>{
    TaskStatus::errorBackoff("d'oh"),
    TaskStatus::done(),
    TaskStatus::incomplete(nullptr),
    overwriteable_status,
    TaskStatus::failed(),
    TaskStatus::done(),  // tests the transition away from failed
  }) {
    worker.recordRunningTaskStatus(rt, TaskStatus::running());
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
    EXPECT_TRUE(worker.recordNonRunningTaskStatus(rt, status, bw.id));
    EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
  }

  // An incorrect invocation ID means that the "not running" status is ignored
  auto bad_rt = rt;
  ++bad_rt.invocationID.startTime;
  worker.recordRunningTaskStatus(rt, TaskStatus::running());
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
  auto status = TaskStatus::done();
  EXPECT_FALSE(worker.recordNonRunningTaskStatus(bad_rt, status, bw.id));
  EXPECT_PCRE_MATCH(glogWarningPattern(), stderr.readIncremental());
  EXPECT_TRUE(worker.recordNonRunningTaskStatus(rt, status, bw.id));
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());

  // Print an error & proceed, when saving one non-running status over another
  EXPECT_TRUE(worker.recordNonRunningTaskStatus(rt, status, bw.id));
  EXPECT_PCRE_MATCH(glogErrorPattern(), stderr.readIncremental());

  // Ignore an overwriteable status on top of a non-overwriteable one
  EXPECT_FALSE(
    worker.recordNonRunningTaskStatus(rt, overwriteable_status, bw.id)
  );
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
}
