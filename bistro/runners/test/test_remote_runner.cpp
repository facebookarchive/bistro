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

#include <folly/Synchronized.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/runners/RemoteWorkerRunner.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/server/HTTPMonitor.h"
#include "bistro/bistro/worker/test/FakeBistroWorkerThread.h"

DECLARE_int32(CAUTION_startup_wait_for_workers);

using namespace std;
using namespace facebook::bistro;
using namespace folly;

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
  )
);
const dynamic kJob = dynamic::object
  ("enabled", true)
  ("owner", "owner")
;

TEST(TestRemoteRunner, HandleResources) {
  FLAGS_CAUTION_startup_wait_for_workers = 0;  // don't wait for workers
  auto task_statuses = make_shared<TaskStatuses>(make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, shared_ptr<Monitor>());

  auto job = make_shared<Job>(*kConfig, "foo_job", kJob);
  auto node1 = make_shared<Node>("test_node1");
  auto node2 = make_shared<Node>("test_node2");
  Synchronized<TaskStatus> status1, status2;
  runner.updateConfig(kConfig);
  auto res = runner.runTask(
    *kConfig,
    job,
    node1,
    nullptr,  // no previous status
    [&status1](const cpp2::RunningTask& rt, TaskStatus&& st) {
      status1->update(rt, std::move(st));
    }
  );
  ASSERT_EQ(TaskRunnerResponse::DoNotRunMoreTasks, res);  // no worker yet
  ASSERT_TRUE(status1->isEmpty());

  // add a worker
  FakeBistroWorkerThread worker("test_worker");
  // to make this worker healthy:
  // 1. registers the worker, and new -> unhealthy after getting running tasks
  runner.processWorkerHeartbeat(
    worker.getBistroWorker(),
    RemoteWorkerUpdate(RemoteWorkerUpdate::UNIT_TEST_TIME, 0)
  );
  sleep(1);  // getting running tasks
  // 2. record healthcheck task, update the timestamp of the last health check
  cpp2::RunningTask rt;
  rt.job = kHealthcheckTaskJob;
  rt.workerShard = "test_worker";
  runner.remoteUpdateStatus(
    rt,
    TaskStatus::done(),
    runner.getSchedulerID(),
    cpp2::BistroInstanceID()
  );
  // 3. with the healthcheck timestamp, another heartbeat to make it healthy
  runner.processWorkerHeartbeat(
    worker.getBistroWorker(),
    RemoteWorkerUpdate(RemoteWorkerUpdate::UNIT_TEST_TIME, 0)
  );
  // now we can run task
  runner.updateConfig(kConfig);  // run this again because we added a new worker
  res = runner.runTask(
    *kConfig,
    job,
    node1,
    nullptr,  // no previous status
    [&status1](const cpp2::RunningTask& rt, TaskStatus&& st) {
      status1->update(rt, std::move(st));
    }
  );
  ASSERT_EQ(TaskRunnerResponse::RanTask, res);
  ASSERT_TRUE(status1->isRunning());
  // can't run another task because of worker resource limit
  res = runner.runTask(
    *kConfig,
    job,
    node2,
    nullptr,  // no previous status
    [&status2](const cpp2::RunningTask& rt, TaskStatus&& st) {
      status2->update(rt, std::move(st));
    }
  );
  ASSERT_EQ(TaskRunnerResponse::DidNotRunTask, res);
  ASSERT_TRUE(status2->isEmpty());
}
