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
#include "bistro/bistro/test/utils.h"
#include "bistro/bistro/worker/test/FakeBistroWorkerThread.h"

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

cpp2::WorkerSetID addToWorkerSetID(
    cpp2::WorkerSetID wsid,
    const FakeBistroWorkerThread& wt) {
  addWorkerIDToHash(&wsid.hash, wt.getBistroWorker().id);
  ++wsid.version;
  return wsid;
}

// Wait for RemoteWorkerRunner to realize that the initial wait is over.
void waitToExitInitialWait(const RemoteWorkerRunner& runner) {
  while (runner.inInitialWaitForUnitTest()) {
    /* sleep override */ this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

cpp2::WorkerSetID addFakeWorker(
    RemoteWorkerRunner* runner,
    std::shared_ptr<const Config> config,
    const FakeBistroWorkerThread& wt,
    cpp2::WorkerSetID wsid) {  // The current set of workers

  folly::test::CaptureFD stderr(2, printString);

  // Connect the worker to the Runner.
  auto res = runner->processWorkerHeartbeat(
    wt.getBistroWorker(), cpp2::WorkerSetID(),  // The 'old' scheduler's set ID
    // Cannot use UNIT_TEST_TIME here or elsewhere, since RemoteWorkerRunner
    // has a background thread that calls updateState with the system time.
    RemoteWorkerUpdate()
  );
  wsid = addToWorkerSetID(wsid, wt);
  EXPECT_EQ(wsid, res.workerSetID);

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

  // The minimum worker_check_interval is 1 second, so send heartbeats to
  // speed up the worker becoming healthy.  We need two -- the first sets
  // RemoteWorker::workerSetID_, the second triggers 'consensus permits'.
  for (int i = 0; i < 2; ++i) {
    runner->processWorkerHeartbeat(
      wt.getBistroWorker(), res.workerSetID,  // As if the worker echoed the ID
      RemoteWorkerUpdate()
    );
  }

  waitForRegexOnFd(&stderr, folly::to<std::string>(
    ".* ", wt.shard(), " became healthy.*"
  ));
  stderr.release();

  // Register the new worker's resources
  runner->updateConfig(config);

  return wsid;
}

const dynamic kJob = dynamic::object
  ("enabled", true)
  ("owner", "owner");

TEST_F(TestRemoteRunner, HandleResources) {
  // Create the worker thread first so that the worker exits *after* the
  // runner does (this avoids harmless error messages about the runner being
  // unable to talk to the worker).
  const auto worker_id = randInstanceID();
  FakeBistroWorkerThread worker(
    "test_worker",
    [&worker_id](cpp2::BistroWorker* w) { w->id = worker_id; }
  );

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

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  addFakeWorker(&runner, kConfig, worker, wsid);

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

TEST_F(TestRemoteRunner, MapLogicalResourcesToCGroupPhysical) {
  // Create the worker first for the same reasons as in HandleResources.
  folly::Promise<folly::Unit> tso_cob_ran;
  const auto worker_id = randInstanceID();
  FakeBistroWorkerThread worker(
    "test_worker",
    [&worker_id](cpp2::BistroWorker* w) { w->id = worker_id; },
    [&](const cpp2::RunningTask& rt, const cpp2::TaskSubprocessOptions& tso) {
      if (rt.job == kHealthcheckTaskJob) {
        return;
      }
      EXPECT_EQ(3, tso.cgroupOptions.cpuShares);  // Tests rounding
      EXPECT_EQ(3072, tso.cgroupOptions.memoryLimitInBytes);
      tso_cob_ran.setValue();
    }
  );

  const auto kConfig = make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", {}))
    ("resources", dynamic::object
      ("worker", dynamic::object
        ("my_ram_gb", dynamic::object("default", 3)("limit", 48))
        ("my_cpu_centicore", dynamic::object("default", 270)("limit", 2000))
      )
    )
    (kPhysicalResources, dynamic::object
      (kRamMB, dynamic::object
        (kLogicalResource, "my_ram_gb")
        (kMultiplyLogicalBy, 1024)
        (kEnforcement, kHard)
      )
      (kCPUCore, dynamic::object
        (kLogicalResource, "my_cpu_centicore")
         // Don't downscale CPU in practice! That loses resolution, whereas
         // cgroups cpu.shares actually works with *relative* values anyhow.
        (kMultiplyLogicalBy, 0.01)
        (kEnforcement, kSoft)
      )
    )
  );

  auto task_statuses =
    std::make_shared<TaskStatuses>(std::make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, std::shared_ptr<Monitor>());

  auto job = std::make_shared<Job>(*kConfig, "job", kJob);
  Node node("node");

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  addFakeWorker(&runner, kConfig, worker, wsid);

  waitToExitInitialWait(runner);
  folly::Promise<folly::Unit> status_cob_ran;
  ASSERT_EQ(TaskRunnerResponse::RanTask, runner.runTask(
    *kConfig,
    job,
    node,
    nullptr,  // no previous status
    [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
      ASSERT_EQ("job", rt.job);
      ASSERT_EQ("node", rt.node);
      ASSERT_TRUE(status.isRunning());
      status_cob_ran.setValue();
    }
  ));
  // Check that both our cobs got executed.
  status_cob_ran.getFuture().get();
  tso_cob_ran.getFuture().get();
}

// Worker => resource => amount
using ResourceMap = std::map<std::string, std::map<std::string, int>>;

// Checks that the specified jobs start tasks on the instance node using the
// specified workers, and that the specified amount of worker resources
// remains after each task start.
void checkTasksRunOnWorkersLeavingResources(
    std::shared_ptr<const Config> config,
    RemoteWorkerRunner* runner,
    std::vector<std::tuple<
      // job name (must be unique), job config, worker name, resources *after*
      std::string, std::string, folly::dynamic, ResourceMap
    >> jobs_on_workers_with_resources) {
  // Run a bunch of different jobs with different resources, and check results.
  for (auto& t : jobs_on_workers_with_resources) {
    folly::Promise<folly::Unit> cob_ran;
    ASSERT_EQ(TaskRunnerResponse::RanTask, runner->runTask(
      *config,
      make_shared<Job>(*config, std::get<0>(t), dynamic::object
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
    auto worker_resources = runner->copyResourcesForUnitTest();
    for (const auto& p_wr : std::get<3>(t)) {
      const auto& r_it = worker_resources.find(p_wr.first);
      ASSERT_NE(worker_resources.end(), r_it);
      for (const auto& p_rv : p_wr.second) {
        auto rid = config->resourceNames.lookup(p_rv.first);
        ASSERT_EQ(p_rv.second, r_it->second.at(rid))
          << std::get<0>(t) << " " << p_wr.first << " " << p_rv.first;
      }
    }
  }
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
  const auto w1_id = randInstanceID();
  FakeBistroWorkerThread worker1("w1", [&w1_id](cpp2::BistroWorker* w) {
    w->id = w1_id;
  });
  const auto w2_id = randInstanceID();
  FakeBistroWorkerThread worker2("w2", [&w2_id](cpp2::BistroWorker* w) {
    w->id = w2_id;
  });

  auto task_statuses = make_shared<TaskStatuses>(make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, shared_ptr<Monitor>());

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  wsid = addFakeWorker(&runner, kConfig, worker1, wsid);
  // Fake that w1 knows about w2, for consensusPermitsBecomingHealthy.
  runner.processWorkerHeartbeat(
    worker1.getBistroWorker(), addToWorkerSetID(wsid, worker2),
    RemoteWorkerUpdate()
  );
  wsid = addFakeWorker(&runner, kConfig, worker2, wsid);

  waitToExitInitialWait(runner);
  checkTasksRunOnWorkersLeavingResources(
    kConfig,
    &runner,
    {
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
    }
  );
}

TEST_F(TestRemoteRunner, WorkerPhysicalResources) {
  const auto kConfig = make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", {}))
    ("resources", dynamic::object
      ("worker", dynamic::object
        // Weight 1 so the "busiest" selector has a sort key.
        // Limit 0 so that no tasks start without physical resources.
        ("ram", dynamic::object("default", 3)("weight", 1)("limit", 0))
        ("cpu", dynamic::object("default", 2)("weight", 1)("limit", 0))
        ("gpu", dynamic::object("default", 1)("weight", 1)("limit", 0))
        // The model-specific GPU resource isn't required by default.
        ("GPU: a", dynamic::object("default", 0)("weight", 0)("limit", 0))
      )
    )
    (kPhysicalResources, dynamic::object
      (kRamMB, dynamic::object
        (kLogicalResource, "ram")
        (kMultiplyLogicalBy, 2)
        // After subtracting reserves, w3 will be too small to run anything.
        (kPhysicalReserveAmount, 1)
      )
      (kCPUCore, dynamic::object(kLogicalResource, "cpu"))
      (kGPUCard, dynamic::object(kLogicalResource, "gpu"))
    )
    // w2 will be smaller, so we will use it first.
    ("remote_worker_selector", "busiest")
  );

  // Like in HandleResources, make the workers before making the runner.
  const auto w1_id = randInstanceID();
  FakeBistroWorkerThread worker1("w1", [&w1_id](cpp2::BistroWorker* w) {
    w->id = w1_id;
    w->usableResources.memoryMB = 10;
    w->usableResources.cpuCores = 2;
    w->usableResources.gpus.emplace_back();
    w->usableResources.gpus.back().name = "a";
  });
  const auto w2_id = randInstanceID();
  FakeBistroWorkerThread worker2("w2", [&w2_id](cpp2::BistroWorker* w) {
    w->id = w2_id;
    w->usableResources.memoryMB = 8;
    w->usableResources.cpuCores = 2;
    w->usableResources.gpus.emplace_back();
    w->usableResources.gpus.back().name = "a";
  });
  const auto w3_id = randInstanceID();
  FakeBistroWorkerThread worker3("w3", [&w3_id](cpp2::BistroWorker* w) {
    w->id = w3_id;
    w->usableResources.memoryMB = 5;  // Minus 1 reserve, over 2 => 2 logical
    w->usableResources.cpuCores = 2;
    w->usableResources.gpus.emplace_back();
    // We have no model-specific resource "GPU: b", so it won't show up.
    w->usableResources.gpus.back().name = "b";
  });

  auto task_statuses = make_shared<TaskStatuses>(make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, shared_ptr<Monitor>());

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  wsid = addFakeWorker(&runner, kConfig, worker1, wsid);
  // Fake that w1 knows about w2, for consensusPermitsBecomingHealthy.
  runner.processWorkerHeartbeat(
    worker1.getBistroWorker(), addToWorkerSetID(wsid, worker2),
    RemoteWorkerUpdate()
  );
  wsid = addFakeWorker(&runner, kConfig, worker2, wsid);
  runner.processWorkerHeartbeat(
    worker2.getBistroWorker(), addToWorkerSetID(wsid, worker3),
    RemoteWorkerUpdate()
  );
  wsid = addFakeWorker(&runner, kConfig, worker3, wsid);

  waitToExitInitialWait(runner);
  checkTasksRunOnWorkersLeavingResources(
    kConfig,
    &runner,
    {
      // This fits on either worker, but w2 started slightly smaller.
      std::make_tuple(
        "j1", "w2", dynamic(dynamic::object),
        ResourceMap{  // RAM is 1 lower due to the reserve setting.
          // Less busy than w2.
          {"w1", {{"ram", 4}, {"cpu", 2}, {"gpu", 1}, {"GPU: a", 1}}},
          // Used by the task.
          {"w2", {{"ram", 0}, {"cpu", 0}, {"gpu", 0}, {"GPU: a", 1}}},
          // Too small for task.
          {"w3", {{"ram", 2}, {"cpu", 2}, {"gpu", 1}, {"GPU: a", 0}}},
        }
      ),
    }
  );
}
