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

void addFakeWorker(
    RemoteWorkerRunner* runner,
    std::shared_ptr<const Config> config,
    const FakeBistroWorkerThread& wt) {

  folly::test::CaptureFD stderr(2, printString);

  // Connect the worker to the Runner.
  runner->processWorkerHeartbeat(
    wt.getBistroWorker(),
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
    cpp2::BistroInstanceID()
  );

  // The minimum worker_check_interval is 1 second, so send another heartbeat.
  runner->processWorkerHeartbeat(
    wt.getBistroWorker(),
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
  FakeBistroWorkerThread worker("test_worker");

  const auto kConfig = make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object
      ("levels", { "level1" , "level2" })
      ("node_source", "range_label")
      ("node_source_prefs", dynamic::object)
    )
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
  auto node1 = make_shared<Node>("test_node1");
  auto node2 = make_shared<Node>("test_node2");

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
  ASSERT_EQ(TaskRunnerResponse::RanTask, runner.runTask(
    *kConfig,
    job,
    node1,
    nullptr,  // no previous status
    [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
      ASSERT_EQ("foo_job", rt.job);
      ASSERT_EQ("test_node1", rt.node);
      ASSERT_TRUE(status.isRunning());
    }
  ));

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
