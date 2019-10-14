/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <thread>

#include <folly/dynamic.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"
#include "bistro/bistro/if/gen-cpp2/BistroWorker.h"
#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/remote/WorkerSetID.h"
#include "bistro/bistro/server/test/ThriftMonitorTestThread.h"
#include "bistro/bistro/test/utils.h"
#include "bistro/bistro/worker/StopWorkerOnSignal.h"
#include "bistro/bistro/worker/test/BistroWorkerTestThread.h"
#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/utils/hostname.h"

DECLARE_int32(heartbeat_period_sec);
DECLARE_int32(incremental_sleep_ms);
DECLARE_int32(CAUTION_worker_suicide_task_kill_wait_ms);

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
    // I don't want to add this to ThriftMonitorTestThread.cpp
    //   ("task_subprocess", dynamic::object("process_group_leader", true))
    // since that could make debugging the test slightly more annoying.
    // Instead, just `exec` so that the killTask() terminates the whole
    // process tree of the task.
    "exec sleep 10000"
};

struct TestWorker : public ::testing::Test {
  TestWorker() {
    // Faster heartbeat arrival => tests finish faster
    FLAGS_heartbeat_period_sec = 0;
    // Speed up HandleSuicide
    FLAGS_CAUTION_worker_suicide_task_kill_wait_ms = 1000;
  }
};


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
  folly::test::CaptureFD stderr(2, printString);
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
  // see Thrift2 mock example in: https://phabricator.intern.facebook.com/D1491659
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
      ASSERT_EQ(4, log.lines.size());
      ASSERT_LE(start_time, log.lines[0].time);
      ASSERT_EQ("test_job", log.lines[0].jobID);
      ASSERT_EQ("test_node", log.lines[0].nodeID);
      EXPECT_EQ("running", parseJson(log.lines[0].line)["event"].asString());
      // Not bothering to verify 1 & 2 since test_task_subprocess_queue
      // and test_local_runner cover these well.
      auto j = parseJson(log.lines[3].line);
      ASSERT_EQ("got_status", j["event"].asString());
      ASSERT_EQ("done", j["raw_status"].asString());
    }
    ASSERT_LE(start_time, log.lines.back().time);
    ASSERT_EQ("test_job", log.lines.back().jobID);
    ASSERT_EQ("test_node", log.lines.back().nodeID);
    ASSERT_EQ(LogLine::kNotALineID, log.nextLineID);
  }
}

bool findInLog(
    BistroWorkerTestThread& worker,
    std::string logtype,
    std::string job,
    std::string node,
    std::string target,
    int num_lines = 10,
    folly::EventBase* evb = nullptr) {
  cpp2::LogLines ll;
  worker.getClient(evb)->sync_getJobLogsByID(
    ll, logtype, {job}, {node}, 0, true, num_lines, ""
  );
  for (const auto& l : ll.lines) {
    if (l.line == target) {
      return true;
    }
  }
  return false;
}

// Shared harness for HandleSuicide and HandleSignal
struct TestSuicide : public TestWorker {
  TestSuicide() : TestWorker(), worker_(
    bind(&ThriftMonitorTestThread::getClient, &sched_, std::placeholders::_1),
    [this](BistroWorkerTestThread* worker, const char* message) noexcept {
      if (!committedSuicide_.load() && strcmp(message, "suicide") == 0) {
        committedSuicide_.store(true);
        // We should now have evidence of both tasks getting SIGTERM.  This
        // cannot run after `loopOnce()` below, since the server will have
        // stopped by then.
        for (size_t i = 0; i < 2; ++i) {
          // We cannot just use the current thread's evb, since
          // sync_getJobLogsByID above expects to be able to drive it, but
          // we are mid-evb-callback already.
          folly::EventBase evb;
          EXPECT_TRUE(findInLog(
            *worker, "stdout", "j", folly::to<std::string>("n", i),
            folly::to<std::string>("TERMn", i, "\n"), /*num_lines=*/ 10, &evb
          ));
        }
      }
    }
  ) {
    // Faster heartbeat arrival => tests finish faster
    FLAGS_heartbeat_period_sec = 0;
    // Speed up HandleSuicide
    FLAGS_CAUTION_worker_suicide_task_kill_wait_ms = 1000;
  }

  void startTasks() {
    // Start two tasks, and wait for them to set signal handlers.
    for (size_t i = 0; i < 2; ++i) {
      const auto node = folly::to<std::string>("n", i);
      cpp2::TaskSubprocessOptions subproc_opts;
      subproc_opts.processGroupLeader = true;  // So we kill the `sleep`
      worker_.runTask("j", node, std::vector<std::string>{
        // Use "" to break up the two strings we want to regex-match, so
        // that we don't match the worker echoing the command.
        "/bin/sh", "-c", folly::to<std::string>(
          "trap 'echo TERM\"\"n", i, "; kill -9 $$' TERM; ",
          "echo j_n", i, "_\"\"ok; sleep 10000"
        )
      }, std::move(subproc_opts));
      // Wait for the task to start.
      while (!findInLog(
        worker_, "stdout", "j", node, folly::to<std::string>("j_n", i, "_ok\n")
      )) {}
      // It has not seen SIGTERM yet.
      EXPECT_FALSE(findInLog(
        worker_, "stdout", "j", node, folly::to<std::string>("TERMn", i, "\n")
      ));
    }
  }

  ThriftMonitorTestThread sched_;
  // The setup mostly follows HandleNormal
  std::atomic_bool committedSuicide_{false};
  BistroWorkerTestThread worker_;
};

TEST_F(TestSuicide, ViaSchedulerRequest) {
  {
    folly::test::CaptureFD stderr(2, printString);
    waitForWorkerHealthy(worker_, &stderr);
  }
  startTasks();

  // TERM-wait-KILL all tasks, and stop the server.  This takes ~1 second
  // since it is not implemented very efficiently.
  worker_.handler()->requestSuicide(
    worker_.getSchedulerID(), worker_.getWorker().id
  );

  // Wait for the tasks to exit, and for the server to stop.
  while (!committedSuicide_.load()) {
    folly::EventBaseManager::get()->getEventBase()->loopOnce();
  }
}

TEST_F(TestSuicide, ViaSignal) {
  folly::test::CaptureFD stderr(2, printString);
  StopWorkerOnSignal signal_handler(
    folly::EventBaseManager::get()->getEventBase(),
    {SIGUSR1},  // NB: FB test infra breaks SIGQUIT?!
    worker_.handler()
  );
  waitForWorkerHealthy(worker_, &stderr);
  startTasks();

  pid_t my_pid = ::getpid();
  PCHECK(my_pid != -1);
  if (::kill(my_pid, SIGUSR1) == -1) {
    PLOG(ERROR) << "Failed to signal " << my_pid;
  } else {
    LOG(INFO) << "Signaled " << my_pid;
  }

  // Wait for the tasks to exit, and for the server to stop.
  while (!committedSuicide_.load()) {
    printString("waiting for signal\n"); // bypass glog for ease of debugging
    stderr.readIncremental();  // dump glog for ease of debugging
    folly::EventBaseManager::get()->getEventBase()->loopOnce(EVLOOP_NONBLOCK);
    /* sleep override */ this_thread::sleep_for(std::chrono::milliseconds(80));
  }
  EXPECT_PCRE_MATCH(folly::to<std::string>(
     ".* Got signal ", SIGUSR1, ", shutting down worker.*"
  ), stderr.read());
}

TEST_F(TestWorker, HandleKillTask) {
  folly::test::CaptureFD stderr(2, printString);
  ThriftMonitorTestThread scheduler;
  BistroWorkerTestThread worker(bind(
    &ThriftMonitorTestThread::getClient, &scheduler, std::placeholders::_1
  ));
  waitForWorkerHealthy(worker, &stderr);

  const size_t kNumTasks = 2;
  cpp2::RunningTask rt[kNumTasks];
  for (int i = 0; i < kNumTasks; i++) {
    rt[i] = worker.runTask("test_job", to<string>("node", i), kSleepCmd);
  }

  vector<cpp2::RunningTask> rts;
  worker.getClient()->sync_getRunningTasks(rts, worker.getWorker().id);
  ASSERT_EQ(kNumTasks, rts.size());

  // Kill one task at a time.
  for (int task_to_kill = 0; task_to_kill < kNumTasks; ++task_to_kill) {
    worker.getClient()->sync_killTask(
      rt[task_to_kill],
      worker.getSchedulerID(),
      worker.getWorker().id,
      cpp2::KillRequest()
    );
    for (int i = 0; i < kNumTasks; i++) {
      cpp2::LogLines log;
      do {  // Wait for the kill to work
        /* sleep override */
        this_thread::sleep_for(std::chrono::milliseconds(10));
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
      } while (i == task_to_kill && log.lines.size() < 4);
      ASSERT_LE(1, log.lines.size());
      EXPECT_EQ("running", parseJson(log.lines[0].line)["event"].asString());
      if (i <= task_to_kill) {
        ASSERT_EQ(4, log.lines.size());
        auto j = folly::parseJson(log.lines[3].line);
        EXPECT_EQ("got_status", j["event"].asString());
        EXPECT_EQ(
          "Task killed, no status returned",
          j["status"]["data"]["exception"].asString()
        );
      } else {
        EXPECT_EQ(1, log.lines.size());
      }
    }
    // Once this event fires, the worker no longer considers the task running.
    auto re = folly::to<std::string>(
      ".* worker task state change: acknowledged_by_scheduler - "
      "test_job / node", task_to_kill, "\n.*"
    );
    waitForRegexOnFd(&stderr, re.c_str());
    worker.getClient()->sync_getRunningTasks(rts, worker.getWorker().id);
    EXPECT_EQ(kNumTasks - task_to_kill - 1, rts.size());
  }
}

template <typename SchedulerT>
struct SchedulerWithWorker {
  SchedulerWithWorker()
    : scheduler_(new SchedulerT()),
      ssit_(scheduler_),
      worker_([this](folly::EventBase* event_base) {
        return make_shared<cpp2::BistroSchedulerAsyncClient>(
          HeaderClientChannel::newChannel(
            async::TAsyncSocket::newSocket(event_base, ssit_.getAddress())
          )
        );
      }) {}

  std::shared_ptr<SchedulerT> scheduler_;
  apache::thrift::ScopedServerInterfaceThread ssit_;
  BistroWorkerTestThread worker_;
};

struct ProtocolVerFakeScheduler : public virtual cpp2::BistroSchedulerSvIf {
  ProtocolVerFakeScheduler() : protocolVersion_(-1) {}  // Start incompatible.
  void processHeartbeat(
      cpp2::SchedulerHeartbeatResponse& res,
      const cpp2::BistroWorker& /*worker*/,
      const cpp2::WorkerSetID& /*worker_set_id*/) override {
    res.protocolVersion = protocolVersion_.copy();
    res.id.startTime = 123;  // The "no scheduler" ID is 0/0, so change it.
    res.workerSetID.schedulerID = res.id;
  }
  folly::Synchronized<int16_t> protocolVersion_;
};

TEST_F(TestWorker, HandleBadProtocolVersion) {
  folly::test::CaptureFD stderr(2, printString);

  SchedulerWithWorker<ProtocolVerFakeScheduler> sw;

  waitForRegexOnFd(&stderr, folly::to<std::string>(
    ".*Unable to send heartbeat to scheduler: Worker-scheduler protocol "
    "version mismatch: ", cpp2::common_constants::kProtocolVersion(),
    " is not compatible with -1.*"
  ).c_str());

  const char* kNewSchedulerRegex = ".* Connected to new scheduler .*";
  EXPECT_NO_PCRE_MATCH(kNewSchedulerRegex, stderr.read());

  sw.scheduler_->protocolVersion_ = cpp2::common_constants::kProtocolVersion();
  waitForRegexOnFd(&stderr, kNewSchedulerRegex);
}

cpp2::BistroInstanceID bistroWorkerID() {
  cpp2::BistroInstanceID id;
  id.startTime = 89342789023740;
  id.rand = 129309723890472;
  return id;
}

struct WorkerSetIDFakeScheduler : public virtual cpp2::BistroSchedulerSvIf {
  WorkerSetIDFakeScheduler() {
    // The "no scheduler" ID is 0/0, so change it.
    workerSetID_->schedulerID.startTime = 123;
    addWorkerIDToHash(&workerSetID_->hash, bistroWorkerID());
  }
  void processHeartbeat(
      cpp2::SchedulerHeartbeatResponse& res,
      const cpp2::BistroWorker& /*worker*/,
      const cpp2::WorkerSetID& worker_set_id) override {
    SYNCHRONIZED (workerSetIDs_) {
      if (!workerSetIDs_.empty()) {
        // Only the first heartbeat can be non-echoed.
        ASSERT_NE(0, worker_set_id.schedulerID.startTime);
      }
      if (workerSetIDs_.empty() || workerSetIDs_.back() != worker_set_id) {
        workerSetIDs_.emplace_back(worker_set_id);
      }
    }
    res.protocolVersion = cpp2::common_constants::kProtocolVersion();
    SYNCHRONIZED (workerSetID_) {
      res.id = workerSetID_.schedulerID;
      res.workerSetID = workerSetID_;
    }
  }
  folly::Synchronized<std::vector<cpp2::WorkerSetID>> workerSetIDs_;
  folly::Synchronized<cpp2::WorkerSetID> workerSetID_;
};

TEST_F(TestWorker, EchoWorkerSetID) {
  SchedulerWithWorker<WorkerSetIDFakeScheduler> sw;

  // Check that the worker echos the initial WorkerSetID.
  while (sw.scheduler_->workerSetIDs_->size() < 2) {
    /* sleep override */ this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SYNCHRONIZED(ids, sw.scheduler_->workerSetIDs_) {
    ASSERT_EQ(2, ids.size());
    EXPECT_EQ(cpp2::WorkerSetID(), ids[0]);  // The worker's ID starts empty
    // Then, it echos what the scheduler gave it.
    EXPECT_EQ(sw.scheduler_->workerSetID_.copy(), ids[1]);
  }

  // Move the version forward.
  sw.scheduler_->workerSetID_->version = 1;
  while (sw.scheduler_->workerSetIDs_->size() < 3) {
    /* sleep override */ this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SYNCHRONIZED(ids, sw.scheduler_->workerSetIDs_) {
    ASSERT_EQ(3, ids.size());
    EXPECT_EQ(1, ids[2].version);
  }

  // Check that the version cannot go backward.
  folly::test::CaptureFD stderr(2, printString);
  sw.scheduler_->workerSetID_->version = 0;
  waitForRegexOnFd(&stderr, ".*scheduler response with older WorkerSetID.*");
  SYNCHRONIZED(ids, sw.scheduler_->workerSetIDs_) {
    ASSERT_EQ(3, ids.size());
    EXPECT_EQ(1, ids[2].version);
  }
}
