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

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/runners/RemoteWorkerRunner.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/WorkerSetID.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/server/HTTPMonitor.h"
#include "bistro/bistro/worker/test/FakeBistroWorkerThread.h"
#include "bistro/bistro/worker/test/utils.h"

DECLARE_int32(CAUTION_startup_wait_for_workers);
DECLARE_int32(incremental_sleep_ms);

using namespace std;
using namespace facebook::bistro;
using namespace folly;

struct TestRemoteRunner : public ::testing::Test {
  TestRemoteRunner() {
    // Don't block starting tasks due to 'initial wait'.
    FLAGS_CAUTION_startup_wait_for_workers = 0;
    // Make BackgroundThreads exit a lot faster.
    FLAGS_incremental_sleep_ms = 10;
    // Minimize delay between RemoteWorkerRunner's updateState calls.
    FLAGS_worker_check_interval = 1;
  }
};

cpp2::BistroInstanceID randInstanceID() {
  cpp2::BistroInstanceID id;
  id.startTime = folly::Random::rand64();
  id.rand = folly::Random::rand64();
  return id;
}

cpp2::WorkerSetID workerSetID(const FakeBistroWorkerThread& wt) {
  cpp2::WorkerSetID ws;  // Leave .version and .schedulerID as default
  addWorkerIDToHash(&ws.hash, wt.getBistroWorker().id);
  return ws;
}

void addFakeWorker(
    RemoteWorkerRunner* runner,
    std::shared_ptr<const Config> config,
    const FakeBistroWorkerThread& wt) {

  folly::test::CaptureFD stderr(2, printString);

  // Connect the worker to the Runner.
  runner->processWorkerHeartbeat(
    wt.getBistroWorker(), workerSetID(wt),
    // Cannot use UNIT_TEST_TIME here or elsewhere, since RemoteWorkerRunner
    // has a background thread that calls updateState with the system time.
    RemoteWorkerUpdate()
  );

  // Wait the Runner to get running tasks & to request a healthcheck, in
  // either order.
  auto kGotRunningTasks = folly::to<std::string>(
    ".* Recorded 0 new running tasks for ", wt.shard(), " .*"
  );
  auto kSentHealthcheck = ".* Sent healthchecks to 1 workers .*";
  waitForRegexOnFd(&stderr, folly::to<std::string>(
    "(", kGotRunningTasks, kSentHealthcheck, "|",
         kSentHealthcheck, kGotRunningTasks, ")"
  ));

  // Fake a reply to the healthcheck -- does not run RemoteWorker::updateState.
  cpp2::RunningTask rt;
  rt.job = kHealthcheckTaskJob;
  rt.workerShard = wt.shard();
  rt.invocationID.startTime = time(nullptr);  // For "time since healthcheck"
  runner->remoteUpdateStatus(
    rt,
    TaskStatus::done(),
    runner->getSchedulerID(),
    wt.getBistroWorker().id
  );

  // The minimum worker_check_interval is 1 second, so send another heartbeat.
  runner->processWorkerHeartbeat(
    wt.getBistroWorker(), workerSetID(wt),
    RemoteWorkerUpdate()
  );

  waitForRegexOnFd(&stderr, folly::to<std::string>(
    ".* ", wt.shard(), " became healthy.*"
  ));
  stderr.release();

  // Register the new worker's resources
  runner->updateConfig(config);
}

const dynamic kJob = dynamic::object
  ("enabled", true)
  ("owner", "owner");

TEST_F(TestRemoteRunner, HandleResources) {
  // Create the worker thread first so that the worker exits *after* the
  // runner does (this avoids harmless error messages about the runner being
  // unable to talk to the worker).
  FakeBistroWorkerThread worker("test_worker", randInstanceID());

  const auto kConfig = make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", {}))
    ("resources", dynamic::object
      ("worker", dynamic::object
        ("concurrency", dynamic::object
          ("default", 1)
          ("limit", 1)
        )
      )
    ));

  auto task_statuses = make_shared<TaskStatuses>(make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, shared_ptr<Monitor>());

  auto job = make_shared<Job>(*kConfig, "foo_job", kJob);
  Node node1("test_node1");
  Node node2("test_node2");

  // Cannot run a task without workers
  runner.updateConfig(kConfig);
  auto res = runner.runTask(
    *kConfig,
    job,
    node1,
    nullptr,  // no previous status
    [](const cpp2::RunningTask&, TaskStatus&&) { FAIL() << "Never runs"; }
  );
  ASSERT_EQ(TaskRunnerResponse::DoNotRunMoreTasks, res);

  addFakeWorker(&runner, kConfig, worker);

  // The first task will consume all of `test_worker`'s `concurrency`.
  {
    folly::Promise<folly::Unit> cob_ran;
    ASSERT_EQ(TaskRunnerResponse::RanTask, runner.runTask(
      *kConfig,
      job,
      node1,
      nullptr,  // no previous status
      [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
        ASSERT_EQ("foo_job", rt.job);
        ASSERT_EQ("test_node1", rt.node);
        ASSERT_TRUE(status.isRunning());
        cob_ran.setValue();
      }
    ));
    cob_ran.getFuture().get();  // Ensure the cob runs.
  }

  // Can't run another task because of the worker resource limit.
  res = runner.runTask(
    *kConfig,
    job,
    node2,
    nullptr,  // no previous status
    [](const cpp2::RunningTask&, TaskStatus&&) { FAIL() << "Never runs"; }
  );
  ASSERT_EQ(TaskRunnerResponse::DidNotRunTask, res);
}

TEST_F(TestRemoteRunner, TestBusiestSelector) {
  const auto kConfig = make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", {}))
    ("resources", dynamic::object
      ("worker", dynamic::object
        ("cheap", dynamic::object
          ("default", 1)
          ("limit", 30)
          ("weight", 1)
        )
        ("expensive", dynamic::object
          ("default", 1)
          ("limit", 3)
          ("weight", 10)
        )
        ("unranked", dynamic::object
          ("default", 1)
          ("limit", 5)
        )
      )
    )
    ("remote_worker_selector", "busiest")
    ("worker_resources_override", dynamic::object
      // The second worker is slightly smaller, so we'll fill it first.
      ("w2", dynamic::object("cheap", 25))
    )
  );

  // Like in HandleResources, make the workers before making the runner.
  FakeBistroWorkerThread worker1("w1", randInstanceID());
  FakeBistroWorkerThread worker2("w2", randInstanceID());

  auto task_statuses = make_shared<TaskStatuses>(make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, shared_ptr<Monitor>());

  addFakeWorker(&runner, kConfig, worker1);
  addFakeWorker(&runner, kConfig, worker2);
  // Wait for RemoteWorkerRunner to realize that the initial wait is over.
  while (runner.inInitialWaitForUnitTest()) {
    /* sleep override */ this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Worker => resource => amount
  using ResourceMap = std::map<std::string, std::map<std::string, int>>;
  // Run a bunch of different jobs with different resources, and check results.
  for (auto& t : std::vector<std::tuple<
    std::string, std::string, folly::dynamic, ResourceMap  // resources *after*
  >>{
    // This fits on either worker, but w2 started slightly smaller.
    std::make_tuple(
      "j1", "w2", dynamic(dynamic::object),
      ResourceMap{
        {"w1", {{"cheap", 30}, {"expensive", 3}, {"unranked", 5}}},
        {"w2", {{"cheap", 24}, {"expensive", 2}, {"unranked", 4}}},
      }
    ),
    // j2 fits on either, so it picks w2 which has less weight.
    std::make_tuple(
      "j2", "w2", dynamic(dynamic::object("expensive", 0)),
      ResourceMap{
        {"w1", {{"cheap", 30}, {"expensive", 3}, {"unranked", 5}}},
        {"w2", {{"cheap", 23}, {"expensive", 2}, {"unranked", 3}}},
      }
    ),
    // j3 must run on w1 since it won't fit on w2 due to "unranked".
    std::make_tuple(
      "j3", "w1", dynamic(dynamic::object("expensive", 2)("unranked", 4)),
      ResourceMap{
        {"w1", {{"cheap", 29}, {"expensive", 1}, {"unranked", 1}}},
        {"w2", {{"cheap", 23}, {"expensive", 2}, {"unranked", 3}}},
      }
    ),
    // j4 through j7 all pick w1 since it continues to have less weight.
    std::make_tuple(
      "j4", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 28}}}}
    ),
    std::make_tuple(
      "j5", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 27}}}}
    ),
    std::make_tuple(
      "j6", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 26}}}}
    ),
    std::make_tuple(
      "j7", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{
        {"w1", {{"cheap", 25}, {"expensive", 1}, {"unranked", 1}}},
        {"w2", {{"cheap", 23}, {"expensive", 2}, {"unranked", 3}}},
      }
    ),
    // j8 only switches to w2 because there's not enough "unranked" on w1.
    std::make_tuple(
      "j8", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 2)),
      ResourceMap{
        {"w1", {{"cheap", 25}, {"expensive", 1}, {"unranked", 1}}},
        {"w2", {{"cheap", 22}, {"expensive", 2}, {"unranked", 1}}},
      }
    ),
    // Continue gnawing at w1, since it still has less weight.
    std::make_tuple(
      "j9", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 24}}}}
    ),
    std::make_tuple(
      "j10", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 23}}}}
    ),
    std::make_tuple(
      "j11", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 22}}}}
    ),
    std::make_tuple(
      "j12", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 21}}}}
    ),
    std::make_tuple(
      "j13", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{
        {"w1", {{"cheap", 20}, {"expensive", 1}, {"unranked", 1}}},
        {"w2", {{"cheap", 22}, {"expensive", 2}, {"unranked", 1}}},
      }
    ),
    // Bounce to w2 since w1 lacks sufficient "expensive"
    std::make_tuple(
      "j14", "w2", dynamic(dynamic::object("expensive", 2)("unranked", 0)),
      ResourceMap{
        {"w1", {{"cheap", 20}, {"expensive", 1}, {"unranked", 1}}},
        {"w2", {{"cheap", 21}, {"expensive", 0}, {"unranked", 1}}},
      }
    ),
    // Now w2 has less weight, so gnaw at it awhile.
    std::make_tuple(
      "j15", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w2", {{"cheap", 20}}}}
    ),
    std::make_tuple(
      "j16", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w2", {{"cheap", 19}}}}
    ),
    std::make_tuple(
      "j17", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w2", {{"cheap", 18}}}}
    ),
    std::make_tuple(
      "j18", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w2", {{"cheap", 17}}}}
    ),
    std::make_tuple(
      "j19", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w2", {{"cheap", 16}}}}
    ),
    // One last switch to w1, consuming the one remaining "expensive".
    std::make_tuple(
      "j20", "w1", dynamic(dynamic::object("cheap", 5)),
      ResourceMap{
        {"w1", {{"cheap", 15}, {"expensive", 0}, {"unranked", 0}}},
        {"w2", {{"cheap", 16}, {"expensive", 0}, {"unranked", 1}}},
      }
    ),
    std::make_tuple(
      "j21", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 14}}}}
    ),
    // Eat the last "unranked" from w2
    std::make_tuple(
      "j22", "w2", dynamic(dynamic::object("expensive", 0)),
      ResourceMap{
        {"w1", {{"cheap", 14}, {"expensive", 0}, {"unranked", 0}}},
        {"w2", {{"cheap", 15}, {"expensive", 0}, {"unranked", 0}}},
      }
    ),
    // Now "w1" will have less weight until it completely runs out.
    std::make_tuple(
      "j23", "w1",
      dynamic(dynamic::object("cheap", 12)("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 2}}}}
    ),
    std::make_tuple(
      "j24", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 1}}}}
    ),
    std::make_tuple(
      "j25", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{{"w1", {{"cheap", 0}}}}
    ),
    // Out of "w1", back to "w2"
    std::make_tuple(
      "j26", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
      ResourceMap{
        {"w1", {{"cheap", 0}, {"expensive", 0}, {"unranked", 0}}},
        {"w2", {{"cheap", 14}, {"expensive", 0}, {"unranked", 0}}},
      }
    ),
  }) {
    folly::Promise<folly::Unit> cob_ran;
    ASSERT_EQ(TaskRunnerResponse::RanTask, runner.runTask(
      *kConfig,
      make_shared<Job>(*kConfig, std::get<0>(t), dynamic::object
        ("enabled", true)
        ("owner", "owner")
        ("resources", std::get<2>(t))
      ),
      Node("test_node"),
      nullptr,  // no previous status
      [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
        ASSERT_EQ(std::get<0>(t), rt.job);
        ASSERT_EQ("test_node", rt.node);
        ASSERT_EQ(std::get<1>(t), rt.workerShard) << std::get<0>(t);
        ASSERT_TRUE(status.isRunning());
        cob_ran.setValue();
      }
    ));
    cob_ran.getFuture().get();  // Ensure the cob runs.
    // Check that the resources are what we expect
    auto worker_resources = runner.copyResourcesForUnitTest();
    for (const auto& p_wr : std::get<3>(t)) {
      const auto& r_it = worker_resources.find(p_wr.first);
      ASSERT_NE(worker_resources.end(), r_it);
      for (const auto& p_rv : p_wr.second) {
        auto rid = kConfig->resourceNames.lookup(p_rv.first);
        ASSERT_EQ(p_rv.second, r_it->second.at(rid))
          << std::get<0>(t) << " " << p_wr.first << " " << p_rv.first;
      }
    }
  }
}
