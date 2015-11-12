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
#include <folly/json.h>

#include "bistro/bistro/processes/TaskSubprocessQueue.h"
#include "bistro/bistro/stats/test/utils.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/LogWriter.h"

/**
 * These tests cover:
 *
 *  - Spinning up one, or multiple tasks, monitored by 1 or several threads.
 *
 *  - All 4 cross-products of "was killed / exited normally" times "no
 *    status / status" below.
 *
 *  - LogWriter receives the lines that tasks write to stderr & stdout.
 *
 *  - Tasks that fail to start.
 *
 *  - An exception is thrown when starting a second copy of a task.
 *
 *  - Killing a process group, as well as kills, which fail to reach the
 *    children since they are *not* sent to the process group.
 *
 *  - (implicitly, via the EXPECT_LE invocations below) StatusCob is only
 *    invoked after *both* the child process exits, and the status pipe
 *    closes.  The EXPECT_LE test is valid, since the cob is invoked right
 *    before the task is removed from the task map, and ~TaskSubprocessQueue
 *    waits for this map to become empty.
 *
 *  - Soft and hard kills. The "soft kill" delay is checked in
 *    TermWaitKillWithStatusKilled, which deliberately fails to exit on the
 *    SIGTERM.  Tasks are only killed if the invocation ID matches.
 *
 *  - Events sent to LogWriter are valid JSON, mandatory fields are present,
 *    all fields have the expected types. In some cases, we explicitly
 *    validate the contents of the JSON fields, and event order.
 *
 *  - Rate limiting of task stdout & stderr log lines.
 *
 * Future:
 *  - Test the canary pipe. Not bothering for now, since its setup is
 *    identical to that of the status pipe, which is well-tested.
 *  - Maybe test parent death signal as well?
 */

DECLARE_int32(task_thread_pool_size);

using namespace facebook::bistro;
using Clock = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Clock>;

namespace { // anonymous

SubprocessStatsChecker statsChecker;

void checkUsageLimits(const SubprocessUsage& usage) {
  statsChecker.checkLimits(usage);
}

double timeDiff(TimePoint a, TimePoint b) {
  return
    std::chrono::duration_cast<std::chrono::duration<double>>(a - b).count();
}
double timeSince(TimePoint t) { return timeDiff(Clock::now(), t); }

}

struct TestLogWriter : public BaseLogWriter {
  struct TaskLogs {
    // These are separate since no reliable sequencing can exist between 3 FDs
    std::vector<std::string> stdout_;
    std::vector<std::string> stderr_;
    std::vector<folly::dynamic> events_;
    std::vector<TimePoint> eventTimes_;
  };

  using TaskLogMap = std::map<std::pair<std::string, std::string>, TaskLogs>;

  explicit TestLogWriter(
    TaskLogMap* tlm = nullptr, bool print_logs = true
  ) : taskToLogs_(tlm), printLogs_(print_logs) {}

  void write(
    LogTable table,
    const std::string& job,
    const std::string& node,
    folly::StringPiece line
  ) override {
    auto* logs =
      taskToLogs_ ? &((*taskToLogs_)[std::make_pair(job, node)]) : nullptr;
    switch (table) {
      case LogTable::STDOUT:
        if (printLogs_) {
          LOG(INFO) << "stdout: " << job << " / " << node << ": " << line;
        }
        if (logs) {
          logs->stdout_.push_back(line.str());
        }
        break;
      case LogTable::STDERR:
        if (printLogs_) {
          LOG(INFO) << "stderr: " << job << " / " << node << ": " << line;
        }
        if (logs) {
          logs->stderr_.push_back(line.str());
        }
        break;
      case LogTable::STATUSES:
        {
          // Always parse, since it's good to verify we always get valid JSON.
          auto d = folly::parseJson(line);

          // Check fields' types, ensure mandatory ones are set.
          EXPECT_TRUE(d.at("event").isString());
          EXPECT_TRUE(d.at("worker_host").isString());
          EXPECT_TRUE(d.at("invocation_start_time").isInt());
          EXPECT_TRUE(d.at("invocation_rand").isInt());
          if (auto* raw_status = d.get_ptr("raw_status")) {
            EXPECT_TRUE(raw_status->isString());
          }
          if (auto* message = d.get_ptr("message")) {
            EXPECT_TRUE(message->isString());
          }
          if (auto* status = d.get_ptr("status")) {
            EXPECT_TRUE(status->isObject());
          }
          if (auto* command = d.get_ptr("command")) {
            EXPECT_TRUE(command->isArray());
          }

          // Don't log since TaskSubprocessQueue::logEvent already does.
          if (logs) {
            logs->events_.push_back(std::move(d));
            logs->eventTimes_.push_back(Clock::now());
          }
        }
        break;
      default:
        FAIL() << "Unknown table: " << static_cast<int>(table);
    }
  }

  TaskLogMap* taskToLogs_;
  bool printLogs_;
};

struct TestTaskSubprocessQueue : public ::testing::Test {
  TestTaskSubprocessQueue() : startTime_(Clock::now()) {
    // Need a default for determinism since some tests change this.
    FLAGS_task_thread_pool_size = 10;
  }

  void runAndKill(const std::string& cmd, cpp2::TaskSubprocessOptions opts) {
    cpp2::RunningTask rt;
    rt.job = "job";
    rt.node = "node";
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>());
    auto runTask = [&]() {
      tsq.runTask(
        rt,
        std::vector<std::string>{"/bin/sh", "-c", cmd, "test_sh"},
        "json_arg",
        ".",
        [](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
          EXPECT_EQ("job", rt.job);
          EXPECT_EQ("node", rt.node);
          EXPECT_EQ(
            TaskStatusBits::Incomplete | TaskStatusBits::UsesBackoff
              | TaskStatusBits::DoesNotAdvanceBackoff,
            status.bits()
          );
          EXPECT_EQ(
            "Task killed, no status returned",
            (*status.data()).at("exception").asString()
          );
        },
        [](const cpp2::RunningTask& rt, SubprocessUsage&& usage){
          EXPECT_EQ("job", rt.job);
          EXPECT_EQ("node", rt.node);
          checkUsageLimits(usage);
        },
        opts
      );
    };
    runTask();
    // Ensure that we can't start the same task twice
    try {
      runTask();
      FAIL() << "Should not be able to double-start a task";
    } catch (const std::runtime_error& ex) {
      EXPECT_PCRE_MATCH("Task already running: .* job .* node .*", ex.what());
    }
    cpp2::KillRequest req;
    req.method = cpp2::KillMethod::KILL;
    // We should fail to kill tasks with the wrong invocation ID.
    ++rt.invocationID.rand;
    EXPECT_THROW(tsq.kill(rt, req), std::runtime_error);
    --rt.invocationID.rand;
    tsq.kill(rt, req);
    EXPECT_GT(1.0, timeSince(startTime_));  // Start & kill take < 1 sec
  }

  // Run a task that tries to handle SIGTERM gracefully by returning 'done'.
  void runAndKillWithStatus(
      cpp2::KillRequest kill_req,
      int initial_sleep,
      int post_kill_sleep,
      TestLogWriter::TaskLogMap* task_to_logs = nullptr) {
    cpp2::RunningTask rt;
    rt.job = "job";
    rt.node = "node";
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>(task_to_logs));
    cpp2::TaskSubprocessOptions opts;
    // If we were a PG leader, the `sleep` would also get the SIGTERM.
    opts.processGroupLeader = false;
    tsq.runTask(
      rt,
      std::vector<std::string>{
        // On SIGTERM, set the status to 'done'; exit after post_kill_sleep
        // sec, or exit with no status after initial_sleep sec.
        "/bin/sh", "-c", folly::to<std::string>(
          "trap 'echo done > $2; sleep ", post_kill_sleep,
          "; kill $SLEEP_PID;' TERM; /bin/sleep ", initial_sleep,
          " & SLEEP_PID=$!; wait"
        ), "test_sh"
      },
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        EXPECT_TRUE(status.isDone());
      },
      [](const cpp2::RunningTask& rt, SubprocessUsage&& usage){
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        checkUsageLimits(usage);
      },
      opts
    );
    tsq.kill(rt, kill_req);
    EXPECT_GT(1.0, timeSince(startTime_));  // Start & kill take < 1 sec
  }

  folly::test::ChangeToTempDir td_;
  TimePoint startTime_;
  cpp2::TaskSubprocessOptions taskOpts_;
};

void checkNormalTaskLogs(
    const TestLogWriter::TaskLogs& logs,
    const std::vector<std::string>& cmd,
    const cpp2::RunningTask& rt) {

  EXPECT_EQ(std::vector<std::string>{"stdout\n"}, logs.stdout_);
  EXPECT_EQ(std::vector<std::string>{"stderr\n"}, logs.stderr_);
  size_t event_idx = 0;
  for (const auto& proto_expected_event : std::vector<folly::dynamic>{
    folly::dynamic::object
      ("event", "running")
      ("command", folly::dynamic(cmd.begin(), cmd.end())),
    // The next two can occur in either order, but this one is likelier:
    folly::dynamic::object("event", "task_pipes_closed"),
    folly::dynamic::object
      ("event", "process_exited")
      ("message", "exited with status 0"),
    folly::dynamic::object
      ("event", "got_status")
      ("status", folly::dynamic::object("result_bits", 4)),
  }) {
    // "pipes closed" and "process exited" can occur in either order, so
    // we'll try to swap them if the default order does not fit.
    const auto* event = &logs.events_[event_idx++];
    bool has_raw_status = event_idx > 1;
    auto getExpectedEvent = [&]() {  // has_raw_status can change
      auto expected_event = proto_expected_event;
      expected_event["worker_host"] = logs.events_[0]["worker_host"];
      expected_event["invocation_start_time"] = rt.invocationID.startTime;
      expected_event["invocation_rand"] = rt.invocationID.rand;
      if (has_raw_status) {
        expected_event["raw_status"] = "done";
      }
      return expected_event;
    };
    // If the common order doesn't work, try swapping the events.
    if ((event_idx == 2 || event_idx == 3) && getExpectedEvent() != *event) {
      // No raw status if the process exits before the pipes are closed.
      has_raw_status = event_idx == 2;
      event = &logs.events_[4 - event_idx];
    }
    EXPECT_EQ(getExpectedEvent(), *event);
  }
  EXPECT_EQ(event_idx, logs.events_.size());
}

TEST_F(TestTaskSubprocessQueue, NormalRun) {
  cpp2::RunningTask rt;
  rt.job = "job";
  rt.node = "node";
  std::vector<std::string> cmd{
    "/bin/sh", "-c",
    "sleep 1; echo stdout; echo stderr 1>&2; echo done > $2", "test_sh"
  };
  TestLogWriter::TaskLogMap task_to_logs;

  int pipe_fd = -1;
  {
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>(&task_to_logs));
    tsq.runTask(
      rt,
      cmd,
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        EXPECT_TRUE(status.isDone());
      },
      [](const cpp2::RunningTask& rt, SubprocessUsage&& usage){
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        checkUsageLimits(usage);
      },
      cpp2::TaskSubprocessOptions()  // defaults should be ok
    );
    pipe_fd = tsq.statusPipeFdForTest();
    EXPECT_GT(1.0, timeSince(startTime_));  // fork + exec should take < 1 sec
  }
  // The process must not exit before 1 second elapses.
  EXPECT_LE(1.0, timeSince(startTime_));

  // Exhaustively check the process's log outputs & events
  EXPECT_EQ(1, task_to_logs.size());
  // The base command should get 3 arguments appended to it:
  cmd.insert(cmd.end(), {
    "node", folly::to<std::string>("/dev/fd/", pipe_fd), "json_arg"
  });
  checkNormalTaskLogs(task_to_logs[std::make_pair("job", "node")], cmd, rt);
}

// A multi-task clone of NormalRun
TEST_F(TestTaskSubprocessQueue, MoreTasksThanThreads) {
  cpp2::RunningTask rt;
  rt.job = "job";
  std::vector<std::string> cmd{
    "/bin/sh", "-c",
    "sleep 1; echo stdout; echo stderr 1>&2; echo done > $2", "test_sh"
  };
  TestLogWriter::TaskLogMap task_to_logs;
  // The general case: many processes per threads, more than one thread.
  FLAGS_task_thread_pool_size = 2;
  const int32_t kNumTasks = 10;

  int pipe_fd = -1;
  {
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>(&task_to_logs));
    for (int i = 0; i < kNumTasks; ++i) {
      auto node = folly::to<std::string>("node", i);
      rt.node = node;
      tsq.runTask(
        rt,
        cmd,
        "json_arg",
        ".",
        [node](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
          EXPECT_EQ("job", rt.job);
          EXPECT_EQ(node, rt.node);
          EXPECT_TRUE(status.isDone());
        },
        [node](const cpp2::RunningTask& rt, SubprocessUsage&& usage){
          EXPECT_EQ("job", rt.job);
          EXPECT_EQ(node, rt.node);
          checkUsageLimits(usage);
        },
        cpp2::TaskSubprocessOptions()  // defaults should be ok
      );
    }
    pipe_fd = tsq.statusPipeFdForTest();
    EXPECT_GT(1.0, timeSince(startTime_));  // fork + exec should take < 1 sec
  }
  // All the processes must not exit before 1 second elapses.
  EXPECT_LE(1.0, timeSince(startTime_));

  // Exhaustively check all the processes' log outputs & events
  EXPECT_EQ(kNumTasks, task_to_logs.size());
  for (int i = 0; i < kNumTasks; ++i) {
    auto node = folly::to<std::string>("node", i);
    auto full_cmd = cmd;
    // The base command should get 3 arguments appended to it:
    full_cmd.insert(full_cmd.end(), {
      node, folly::to<std::string>("/dev/fd/", pipe_fd), "json_arg"
    });
    checkNormalTaskLogs(
      task_to_logs[std::make_pair("job", node)], full_cmd, rt
    );
  }
}

TEST_F(TestTaskSubprocessQueue, NoStatus) {
  {
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>());
    cpp2::RunningTask rt;
    rt.job = "job";
    rt.node = "node";
    tsq.runTask(
      rt,
      std::vector<std::string>{"/bin/echo"},
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        EXPECT_EQ(
          TaskStatusBits::Error | TaskStatusBits::UsesBackoff,
          status.bits()
        );
        EXPECT_EQ(
          "Failed to read a status",
          (*status.data()).at("exception").asString()
        );
      },
      [](const cpp2::RunningTask& rt, SubprocessUsage&& usage){
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        checkUsageLimits(usage);
      },
      cpp2::TaskSubprocessOptions()  // defaults should be ok
    );
  }
  EXPECT_GT(1.0, timeSince(startTime_));  // fork+exec+wait should take < 1 sec
}

TEST_F(TestTaskSubprocessQueue, KillSingle) {
  // `sleep` replaces `sh`, gets the SIGKILL, and quits quickly.
  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = false;
  runAndKill("exec sleep 3600", opts);
  EXPECT_GT(1.0, timeSince(startTime_));  // Start, kill & wait take < 1 sec
}

TEST_F(TestTaskSubprocessQueue, KillFailsToReachChild) {
  // Since we neither exec, nor make `sh` a process group leader, `sleep` is
  // never killed.  We must wait for it to close the status pipe FD.
  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = false;
  // A simple "sleep 1" gets `exec`ed by some `sh`s
  runAndKill("sleep 1 ; sleep 3600", opts);
  EXPECT_LE(1.0, timeSince(startTime_));
}

TEST_F(TestTaskSubprocessQueue, ProcGroupKillsChild) {
  // Making a process group lets us kill both `sh` and `sleep` quickly.
  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = true;
  // A simple "sleep 1" gets `exec`ed by some `sh`s
  runAndKill("sleep 3600 ; sleep 3600", opts);
  EXPECT_GT(1.0, timeSince(startTime_));
}

TEST_F(TestTaskSubprocessQueue, FailsToStart) {
  {
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>());
    tsq.runTask(
      {},
      std::vector<std::string>{"/should/not/work/"},
      "json_arg",
      ".",
      [](const cpp2::RunningTask&, TaskStatus&& status) noexcept {
        // Cannot evaluate isInBackoff() since TaskStatus::backoffDuration
        // is only set once this is processed by TaskStatusSnapshot.
        EXPECT_EQ(
          TaskStatusBits::Error | TaskStatusBits::UsesBackoff, status.bits()
        );
        EXPECT_PCRE_MATCH(
          ".* failed to execute /should/not/work/: No such file or directory",
          (*status.data()).at("exception").asString()
        );
      },
      [](const cpp2::RunningTask&, SubprocessUsage&& usage){
        checkUsageLimits(usage);
      },
      cpp2::TaskSubprocessOptions()  // defaults should be ok
    );
  }
  EXPECT_GT(1.0, timeSince(startTime_));  // fork+exec+wait should take < 1 sec
}

TEST_F(TestTaskSubprocessQueue, TermWaitKillWithStatusExited) {
  cpp2::KillRequest req;
  req.method = cpp2::KillMethod::TERM_WAIT_KILL;
  req.killWaitMs = 3600000;  // SIGKILL in 1 hour
  runAndKillWithStatus(req, 3600, 1);  // Sleep 1 hour, or 1 second after TERM
  EXPECT_LE(1.0, timeSince(startTime_));  // Runs for min(3600, 1)
}

TEST_F(TestTaskSubprocessQueue, TermWithStatusExited) {
  cpp2::KillRequest req;
  req.method = cpp2::KillMethod::TERM;
  runAndKillWithStatus(req, 3600, 1);  // Sleep 1 hour, or 1 second after TERM
  EXPECT_LE(1.0, timeSince(startTime_));  // Runs for min(3600, 1)
}

TEST_F(TestTaskSubprocessQueue, TermWaitKillWithStatusKilled) {
  TestLogWriter::TaskLogMap task_to_logs;
  // The SIGTERM 'trap' callback will be SIGKILLed while it sleeps.
  // Sleep 2 seconds, or 2 seconds after TERM
  cpp2::KillRequest req;
  req.method = cpp2::KillMethod::TERM_WAIT_KILL;
  req.killWaitMs = 1000;  // SIGKILL in 1 second
  runAndKillWithStatus(req, 2, 2, &task_to_logs);
  EXPECT_LE(2.0, timeSince(startTime_));  // The original 2-second sleep runs.

  // Validate the event log and capture soft-kill timings
  TimePoint process_exit_time, pipes_closed_time;
  EXPECT_EQ(1, task_to_logs.size());
  auto& logs = task_to_logs[std::make_pair("job", "node")];
  EXPECT_EQ(0, logs.stdout_.size());
  EXPECT_EQ(0, logs.stderr_.size());
  size_t event_idx = 0;
  for (auto& expected_event : std::vector<folly::dynamic>{
    folly::dynamic::object
      ("event", "running")
      ("command", {}),  // I'm too lazy to check the command's contents.
    folly::dynamic::object
      ("event", "process_exited")
      ("raw_status", "done")
      // Since the task produced its own status, Bistro treats it as if it
      // succeded -- this message is the only evidence of the SIGKILL.
      ("message", "killed by signal 9"),
    folly::dynamic::object
      ("event", "task_pipes_closed")
      ("raw_status", "done"),
    folly::dynamic::object
      ("event", "got_status")
      ("raw_status", "done")
      ("status", folly::dynamic::object("result_bits", 4)),
  }) {
    auto event = logs.events_[event_idx++];
    event.erase("worker_host");
    event.erase("invocation_start_time");
    event.erase("invocation_rand");
    if (auto* cmd = event.get_ptr("command")) {
      cmd->erase(cmd->begin(), cmd->end());
    }
    EXPECT_EQ(expected_event, event);
    if (event["event"] == "process_exited") {
      process_exit_time = logs.eventTimes_[event_idx - 1];
    }
    if (event["event"] == "task_pipes_closed") {
      pipes_closed_time = logs.eventTimes_[event_idx - 1];
    }
  }
  EXPECT_EQ(event_idx, logs.events_.size());

  // Check soft-kill timings
  EXPECT_NE(TimePoint(), process_exit_time);  // initialized
  EXPECT_NE(TimePoint(), pipes_closed_time);  // initialized
  // The SIGKILL happens just over 1 sec after start
  EXPECT_LE(1.0, timeDiff(process_exit_time, startTime_));
  // The `sleep` child process waits the full 2 sec, and holds the pipe open.
  EXPECT_LE(2.0, timeDiff(pipes_closed_time, startTime_));
  // There should be a good gap between the SIGKILL and the sleep's exit.
  EXPECT_LE(0.5, timeDiff(pipes_closed_time, process_exit_time));
}

TEST_F(TestTaskSubprocessQueue, RateLimitLog) {
  cpp2::RunningTask rt;
  rt.job = "job";
  rt.node = "node";
  const int32_t kNumIters = 1000;
  std::vector<std::string> cmd{"/bin/bash", "-c", folly::to<std::string>(
    "for ((i=0;i<", kNumIters,  // plain `sh` lacks this kind of `for`
    ";++i)); do echo stderr 1>&2; echo stdout; echo stdout; done; "
    "echo done > $2"
  ), "test_sh"};
  cpp2::TaskSubprocessOptions opts;
  opts.maxLogLinesPerPollInterval = (3 /*lines*/ * kNumIters)
    / (1000 /*ms per sec*/ / opts.pollMs);  // Finish in 1 second.
  {
    TaskSubprocessQueue tsq(folly::make_unique<TestLogWriter>(
      /*task_to_logs=*/ nullptr, /*print_logs=*/ false
    ));
    tsq.runTask(
      rt,
      cmd,
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        EXPECT_TRUE(status.isDone());
      },
      [](const cpp2::RunningTask& rt, SubprocessUsage&& usage){
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
        checkUsageLimits(usage);
      },
      opts
    );
    EXPECT_GT(1.0, timeSince(startTime_));  // fork + exec should take < 1 sec
  }
  // The process must not exit before 1 second elapses.
  auto runtime = timeSince(startTime_);
  EXPECT_LE(1.0, runtime);  // Should be throttled to run for 1 sec.
  EXPECT_GT(1.5, runtime);  // Shouldn't take too long to print 3k lines.
}
