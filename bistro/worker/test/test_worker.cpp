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
#include <thread>

#include <folly/dynamic.h>
#include <folly/json.h>

#include "bistro/bistro/server/test/ThriftMonitorTestThread.h"
#include "bistro/bistro/worker/test/BistroWorkerTestThread.h"
#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/if/gen-cpp2/BistroWorker.h"

DECLARE_int32(heartbeat_period_sec);

using namespace facebook::bistro;
using namespace folly;
using namespace std;

const auto kNormalCmd = vector<string>{
    "/bin/sh",
    "-c",
    "echo my_stdout && echo my_stderr 1>&2 && echo done > $2",
    "test_job"
};

const auto kSleepCmd = vector<string>{
    "/bin/sh",
    "-c",
    // need exec here because kill does not work with subprocesses currently
    "exec sleep 100"
};

TEST(TestWorker, HandleNormal) {
  FLAGS_heartbeat_period_sec = 1; // minimum wait, using 0 would get socket err
  ThriftMonitorTestThread scheduler;
  BistroWorkerTestThread worker(&scheduler);
  // initial state is NEW
  EXPECT_EQ(RemoteWorkerState::State::NEW, worker.getState());
  sleep(3); // takes about 1s to get healthcheck task sent/processed/notified
  EXPECT_EQ(RemoteWorkerState::State::HEALTHY, worker.getState());

  // TODO 5486195 run the following test with a mocked scheduler for
  // 1) faster unit test
  // 1) no error msg for inconsistent running tasks between worker and scheduler
  // 2) checking statusUpdate calls to the scheduler
  // see Thrift2 mock example in: https://phabricator.fb.com/D1491659
  //
  // TODO 5486195 make a real integrated test using a shell script
  auto start_time = time(nullptr);
  worker.runTask("test_job", "test_node", kNormalCmd);
  sleep(1); // wait for the task to finish

  for (const auto& logtype : vector<string>{"stdout", "stderr", "statuses"}) {
    cpp2::LogLines log;
    worker.getClient()->sync_getJobLogsByID(
      log,
      logtype,
      vector<string>({"test_job"}),
      vector<string>({"test_node"}),
      0,
      true,
      10,
      ""
    );
    if (logtype != "statuses") {
      ASSERT_EQ(1, log.lines.size());
      ASSERT_EQ("my_" + logtype + "\n", log.lines.back().line);
    } else {
      ASSERT_EQ(3, log.lines.size());
      ASSERT_LE(start_time, log.lines[0].time);
      ASSERT_EQ("test_job", log.lines[0].jobID);
      ASSERT_EQ("test_node", log.lines[0].nodeID);
      EXPECT_EQ(
        dynamic(dynamic::object
          ("result", "running")
          ("data", dynamic::object("worker_host", getLocalHostName()))),
        parseJson(log.lines[0].line)
      );
      ASSERT_EQ("exited", log.lines[1].line);
      ASSERT_EQ("done", log.lines[2].line);
    }
    ASSERT_LE(start_time, log.lines.back().time);
    ASSERT_EQ("test_job", log.lines.back().jobID);
    ASSERT_EQ("test_node", log.lines.back().nodeID);
    ASSERT_EQ(LogLine::kNotALineID, log.nextLineID);
  }
}

TEST(TestWorker, HandleKillTask) {
  FLAGS_heartbeat_period_sec = 1; // minimum wait, using 0 would get socket err
  ThriftMonitorTestThread scheduler;
  BistroWorkerTestThread worker(&scheduler);
  sleep(3); // takes about 1s to get healthcheck task sent/processed/notified

  cpp2::RunningTask rt[2];
  for (int i=0; i<2; i++) {
    rt[i] = worker.runTask("test_job", to<string>("node", i), kSleepCmd);
  }

  vector<cpp2::RunningTask> rts;
  worker.getClient()->sync_getRunningTasks(rts, worker.getWorker().id);
  ASSERT_EQ(2, rts.size());


  worker.getClient()->sync_killTask(
    rt[0],
    cpp2::KilledTaskStatusFilter::NONE,
    worker.getSchedulerID(),
    worker.getWorker().id
  );
  for (int i=0; i<2; i++) {
    cpp2::LogLines log;
    worker.getClient()->sync_getJobLogsByID(
      log,
      "statuses",
      vector<string>({"test_job"}),
      vector<string>({rt[i].node}),
      0,
      true,
      10,
        ""
    );
    EXPECT_EQ(
      dynamic(dynamic::object
        ("result", "running")
        ("data", dynamic::object("worker_host", getLocalHostName()))),
      parseJson(log.lines[0].line)
    );
    if (i == 0) {
      ASSERT_EQ(3, log.lines.size());
      ASSERT_EQ("soft-killed", log.lines[1].line);
    } else {
      ASSERT_EQ(1, log.lines.size());
    }
  }
  sleep(1);  // wait for notifyFinished() to update runningTasks_
  worker.getClient()->sync_getRunningTasks(rts, worker.getWorker().id);
  ASSERT_EQ(1, rts.size());
}
