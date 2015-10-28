/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <boost/regex.hpp>
#include <gtest/gtest.h>
#include <thread>

#include <folly/dynamic.h>
#include <folly/experimental/TestUtil.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"
#include "bistro/bistro/if/gen-cpp2/BistroWorker.h"
#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/server/test/ThriftMonitorTestThread.h"
#include "bistro/bistro/worker/test/BistroWorkerTestThread.h"
#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/utils/hostname.h"

DECLARE_int32(heartbeat_period_sec);
DECLARE_int32(worker_threads);
DECLARE_int32(incremental_sleep_ms);

using namespace facebook::bistro;
using namespace folly;
using namespace std;
using namespace apache::thrift;

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

struct TestWorker : public ::testing::Test {
  TestWorker() {
    // Faster heartbeat arrival => tests finish faster
    FLAGS_heartbeat_period_sec = 0;
    // Don't waste time spinning up and tearing down 100 threads.
    FLAGS_worker_threads = 2;
    // Make BackgroundThreads exit a lot faster.
    FLAGS_incremental_sleep_ms = 10;
  }
};

// Reads incrementally from fd until the entirety what we have consumed on
// this run matches the given regex.  Caveat: this can easily consume more
// than you intended, preventing your next wait from matching.
void waitForRegexOnFd(folly::test::CaptureFD* fd, const char* regex) {
  std::string all;
  do {
    all += fd->readIncremental();  // So that ChunkCob fires incrementally
  } while (!boost::regex_match(all, boost::regex(regex)));
}

void echoChunks(folly::StringPiece s) {
  if (!s.empty()) {
    std::cout << "stderr: " << s << std::flush;
  }
}

// This is racy (even as written), and basically impossible to get right.
// There are three things at play: (i) both the worker and the scheduler
// have to register the worker as healthy, since either one can fail to
// start tasks, (ii) the two transitions to "healthy" can happen in either
// order, (iii) since the worker takes as truth the status received from the
// scheduler in heartbeat responses, it's possible for the scheduler's
// "unhealthy" response to take a while to arrive to the worker -- even
// arriving after both the scheduler and the worker think that the worker is
// healthy.  The latter point means that the worker can ping-pong "healthy"
// -> "unhealthy" -> "healthy".  Future: maybe the worker shouldn't listen
// to *all* the scheduler's heartbeat responses (e.g.  sequence numbers or
// timeouts could help, or it could only allow transitions from NEW status).
void waitForWorkerHealthy(
    const BistroWorkerTestThread& worker,
    folly::test::CaptureFD* fd) {
  waitForRegexOnFd(fd, ".* Worker [^\n]* became healthy\n.*");
  while (worker.getState() != RemoteWorkerState::State::HEALTHY) {
    // Must sleep to avoid lock contention
    /* sleep override */this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

TEST_F(TestWorker, HandleNormal) {
  folly::test::CaptureFD stderr(2, echoChunks);
  ThriftMonitorTestThread scheduler;
  BistroWorkerTestThread worker(bind(
    &ThriftMonitorTestThread::getClient, &scheduler, std::placeholders::_1
  ));

  // Caution: one cannot safely assert that the initial state is NEW, since
  // on loaded systems, the test thread may fall behind and witness
  // "UNHEALTHY" or even "HEALTHY" instead.
  waitForWorkerHealthy(worker, &stderr);

  // TODO 5486195 run the following test with a mocked scheduler for
  // 1) faster unit test
  // 1) no error msg for inconsistent running tasks between worker and scheduler
  // 2) checking statusUpdate calls to the scheduler
  // see Thrift2 mock example in: https://phabricator.fb.com/D1491659
  //
  // TODO 5486195 make a real integrated test using a shell script
  auto start_time = time(nullptr);
  worker.runTask("test_job", "test_node", kNormalCmd);
  waitForRegexOnFd(  // Wait for the task to finish
    &stderr,
    ".* worker task state change: completed_task - test_job / test_node\n.*"
  );
  stderr.release();

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

TEST_F(TestWorker, HandleKillTask) {
  folly::test::CaptureFD stderr(2, echoChunks);
  ThriftMonitorTestThread scheduler;
  BistroWorkerTestThread worker(bind(
    &ThriftMonitorTestThread::getClient, &scheduler, std::placeholders::_1
  ));
  waitForWorkerHealthy(worker, &stderr);

  cpp2::RunningTask rt[2];
  for (int i=0; i<2; i++) {
    rt[i] = worker.runTask("test_job", to<string>("node", i), kSleepCmd);
  }

  vector<cpp2::RunningTask> rts;
  worker.getClient()->sync_getRunningTasks(rts, worker.getWorker().id);
  ASSERT_EQ(2, rts.size());

  worker.getClient()->sync_killTask(
    rt[0],
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

  // Once this event fires, the worker's no longer considers the task running.
  waitForRegexOnFd(
    &stderr,
    ".* worker task state change: acknowledged_by_scheduler - "
    "test_job / node0\n.*"
  );
  stderr.release();

  worker.getClient()->sync_getRunningTasks(rts, worker.getWorker().id);
  ASSERT_EQ(1, rts.size());
}

struct FakeBistroScheduler : public virtual cpp2::BistroSchedulerSvIf {
  FakeBistroScheduler() : protocolVersion_(-1) {}  // Incompatible by default.
  void processHeartbeat(
      cpp2::SchedulerHeartbeatResponse& res,
      const cpp2::BistroWorker& worker) override {
    res.id.startTime = 123;  // The "no scheduler" ID is 0/0, so change it.
    res.protocolVersion = protocolVersion_.copy();
  }
  folly::Synchronized<int16_t> protocolVersion_;
};

TEST_F(TestWorker, HandleBadProtocolVersion) {
  folly::test::CaptureFD stderr(2, echoChunks);

  auto scheduler = std::make_shared<FakeBistroScheduler>();
  apache::thrift::ScopedServerInterfaceThread ssit_(scheduler);

  BistroWorkerTestThread worker([&](folly::EventBase* event_base) {
    return make_shared<cpp2::BistroSchedulerAsyncClient>(
      HeaderClientChannel::newChannel(
        async::TAsyncSocket::newSocket(event_base, ssit_.getAddress())
      )
    );
  });

  waitForRegexOnFd(&stderr, folly::to<std::string>(
    ".*Unable to send heartbeat to scheduler: Worker-scheduler protocol "
    "version mismatch: ", cpp2::common_constants::kProtocolVersion(),
    " is not compatible with -1.*"
  ).c_str());

  const char* kNewSchedulerRegex = ".* Connected to new scheduler .*";
  EXPECT_NO_PCRE_MATCH(kNewSchedulerRegex, stderr.read());

  scheduler->protocolVersion_ = cpp2::common_constants::kProtocolVersion();
  waitForRegexOnFd(&stderr, kNewSchedulerRegex);
}
