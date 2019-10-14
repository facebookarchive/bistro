/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>
#include <folly/json.h>

#include "bistro/bistro/physical/test/utils.h"
#include "bistro/bistro/processes/TaskSubprocessQueue.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/test/utils.h"
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
DECLARE_int32(incremental_sleep_ms);

using namespace facebook::bistro;
using folly::dynamic;

struct TestLogWriter : public BaseLogWriter {
  struct TaskLogs {
    // These are separate since no reliable sequencing can exist between 3 FDs
    std::vector<std::string> stdout_;
    std::vector<std::string> stderr_;
    std::vector<dynamic> events_;
    std::vector<TestTimePoint> eventTimes_;
  };

  using TaskLogMap = std::map<std::pair<std::string, std::string>, TaskLogs>;

  explicit TestLogWriter(
    folly::Synchronized<TaskLogMap>* tlm = nullptr, bool print_logs = true
  ) : taskToLogs_(tlm), printLogs_(print_logs) {}

  void write(
    LogTable table,
    const std::string& job,
    const std::string& node,
    folly::StringPiece line
  ) override {
    auto record_logs_fn = [&](std::function<void(TaskLogs*)> cob) {
      if (taskToLogs_) {
        SYNCHRONIZED(task_to_logs, *taskToLogs_) {
          cob(&(task_to_logs[std::make_pair(job, node)]));
        }
      }
    };
    switch (table) {
      case LogTable::STDOUT:
        if (printLogs_) {
          LOG(INFO) << "stdout: " << job << " / " << node << ": " << line;
        }
        record_logs_fn([line](TaskLogs* logs) {
          logs->stdout_.push_back(line.str());
        });
        break;
      case LogTable::STDERR:
        if (printLogs_) {
          LOG(INFO) << "stderr: " << job << " / " << node << ": " << line;
        }
        record_logs_fn([line](TaskLogs* logs) {
          logs->stderr_.push_back(line.str());
        });
        break;
      case LogTable::EVENTS:
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
          record_logs_fn([&d](TaskLogs* logs) {
            logs->events_.push_back(std::move(d));
            logs->eventTimes_.push_back(TestClock::now());
          });
        }
        break;
      default:
        FAIL() << "Unknown table: " << static_cast<int>(table);
    }
  }

  // write() is required to be thread-safe
  folly::Synchronized<TaskLogMap>* taskToLogs_;
  const bool printLogs_;
};

struct TestTaskSubprocessQueue : public ::testing::Test {
  TestTaskSubprocessQueue() : startTime_(TestClock::now()) {
    // Need a default for determinism since some tests change this.
    FLAGS_task_thread_pool_size = 10;
  }

  // Default RunningTask used by runTask, runAndKill, etc.
  static cpp2::RunningTask runningTask() {
    cpp2::RunningTask rt;
    rt.job = "job";
    rt.node = "node";
    // CGroup namind depends on this, but most other tests don't care.
    rt.workerShard = "shard";
    return rt;
  }

  static cpp2::KillRequest requestSigkill() {
    cpp2::KillRequest req;
    req.method = cpp2::KillMethod::KILL;
    return req;
  }

  // Status callback asserting the task was killed.
  static void expectKilled(
      const cpp2::RunningTask& rt,
      TaskStatus&& status) noexcept {
    EXPECT_EQ(runningTask(), rt);
    EXPECT_EQ(
      TaskStatusBits::Incomplete | TaskStatusBits::UsesBackoff
        | TaskStatusBits::DoesNotAdvanceBackoff,
      status.bits()
    );
    EXPECT_EQ(
      "Task killed, no status returned",
      (*status.dataThreadUnsafe()).at("exception").asString()
    );
  }

  // Status callback asserting the task had an error matching this regex.
  static void expectErrorRegex(
      std::string regex,
      const cpp2::RunningTask& rt,
      TaskStatus&& st) noexcept {
    EXPECT_EQ(runningTask(), rt);
    EXPECT_EQ(TaskStatusBits::Error | TaskStatusBits::UsesBackoff, st.bits());
    EXPECT_PCRE_MATCH(
      regex, (*st.dataThreadUnsafe()).at("exception").asString()
    );
  }

  // Run a task which will expect to be killed.
  void runTask(
      TaskSubprocessQueue* tsq,
      const std::string& cmd,
      cpp2::TaskSubprocessOptions opts,
      TaskSubprocessQueue::StatusCob status_cob) {
    tsq->runTask(
      runningTask(),
      std::vector<std::string>{"/bin/sh", "-c", cmd, "test_sh"},
      "json_arg",
      ".",
      status_cob,
      [this](const cpp2::RunningTask& rt, cpp2::TaskPhysicalResources&&) {
        EXPECT_EQ("job", rt.job);
        EXPECT_EQ("node", rt.node);
      },
      std::move(opts)
    );
  }

  // Run a task and ensure it can be killed immediately.
  void runAndKill(const std::string& cmd, cpp2::TaskSubprocessOptions opts) {
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>());
    runTask(&tsq, cmd, std::move(opts), &expectKilled);
    // Ensure that we can't start the same task twice
    try {
      runTask(&tsq, cmd, std::move(opts), &expectKilled);
      FAIL() << "Should not be able to double-start a task";
    } catch (const std::runtime_error& ex) {
      EXPECT_PCRE_MATCH("Task already running: .* job .* node .*", ex.what());
    }
    // We should fail to kill tasks with the wrong invocation ID.
    auto rt = runningTask();
    ++rt.invocationID.rand;
    EXPECT_THROW(tsq.kill(rt, requestSigkill()), std::runtime_error);
    --rt.invocationID.rand;
    tsq.kill(rt, requestSigkill());
    EXPECT_GT(1.0, timeSince(startTime_));  // Start & kill take < 1 sec
  }

  // Run a task that tries to handle SIGTERM gracefully by returning 'done'.
  void runAndKillWithStatus(
      cpp2::KillRequest kill_req,
      int initial_sleep,
      int post_kill_sleep,
      folly::Synchronized<TestLogWriter::TaskLogMap>* task_to_logs = nullptr) {
    cpp2::RunningTask rt;
    rt.job = "job";
    rt.node = "node";
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(task_to_logs));
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
      [](const cpp2::RunningTask& rt2, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
        EXPECT_TRUE(status.isDone());
      },
      [this](const cpp2::RunningTask& rt2, cpp2::TaskPhysicalResources&&) {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
      },
      opts
    );
    tsq.kill(rt, kill_req);
    EXPECT_GT(1.0, timeSince(startTime_));  // Start & kill take < 1 sec
  }

  folly::test::ChangeToTempDir td_;
  TestTimePoint startTime_;
  cpp2::TaskSubprocessOptions taskOpts_;
};

void checkNormalTaskLogs(
    const TestLogWriter::TaskLogs& logs,
    const std::vector<std::string>& cmd) {

  EXPECT_EQ(std::vector<std::string>{"stdout\n"}, logs.stdout_);
  EXPECT_EQ(std::vector<std::string>{"stderr\n"}, logs.stderr_);
  size_t event_idx = 0;
  for (const auto& proto_expected_event : std::vector<dynamic>{
    dynamic::object
      ("event", "running")
      ("command", dynamic(cmd.begin(), cmd.end())),
    // The next two can occur in either order, but this one is likelier:
    dynamic::object("event", "task_pipes_closed"),
    dynamic::object
      ("event", "process_exited")
      ("message", "exited with status 0"),
    dynamic::object
      ("event", "got_status")
      ("status", dynamic::object("result_bits", 4)),
  }) {
    // "pipes closed" and "process exited" can occur in either order, so
    // we'll try to swap them if the default order does not fit.
    const auto* event = &logs.events_[event_idx++];
    bool has_raw_status = event_idx > 1;
    auto getExpectedEvent = [&]() {  // has_raw_status can change
      auto expected_event = proto_expected_event;
      expected_event["worker_host"] = logs.events_[0]["worker_host"];
      expected_event["invocation_start_time"] = 0;
      expected_event["invocation_rand"] = 0;
      if (has_raw_status) {
        expected_event["raw_status"] = "done";
      }
      return expected_event;
    };
    // If the common order doesn't work, try swapping the events.
    if ((event_idx == 2 || event_idx == 3) && getExpectedEvent() != *event) {
      event = &logs.events_[4 - event_idx];
      // We MUST have raw status once the pipes are closed, and we MAY
      // have it when the process exits.
      has_raw_status = event_idx == 2 || event->count("raw_status");
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
  folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;

  int pipe_fd = -1;
  {
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(&task_to_logs));
    tsq.runTask(
      rt,
      cmd,
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt2, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
        EXPECT_TRUE(status.isDone());
      },
      [this](const cpp2::RunningTask& rt2, cpp2::TaskPhysicalResources&&) {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
      },
      cpp2::TaskSubprocessOptions()  // defaults should be ok
    );
    pipe_fd = tsq.statusPipeFdForTest();
    EXPECT_GT(1.0, timeSince(startTime_));  // fork + exec should take < 1 sec
  }
  // The process must not exit before 1 second elapses.
  EXPECT_LE(1.0, timeSince(startTime_));

  // Exhaustively check the process's log outputs & events
  SYNCHRONIZED(task_to_logs) {
    EXPECT_EQ(1, task_to_logs.size());
    // The base command should get 3 arguments appended to it:
    cmd.insert(cmd.end(), {
      "node", folly::to<std::string>("/dev/fd/", pipe_fd), "json_arg"
    });
    checkNormalTaskLogs(task_to_logs[std::make_pair("job", "node")], cmd);
  }
}

// A multi-task clone of NormalRun
TEST_F(TestTaskSubprocessQueue, MoreTasksThanThreads) {
  cpp2::RunningTask rt;
  rt.job = "job";
  std::vector<std::string> cmd{
    "/bin/sh", "-c",
    "sleep 1; echo stdout; echo stderr 1>&2; echo done > $2", "test_sh"
  };
  folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;
  // The general case: many processes per threads, more than one thread.
  FLAGS_task_thread_pool_size = 2;
  const int32_t kNumTasks = 10;

  int pipe_fd = -1;
  {
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(&task_to_logs));
    for (int i = 0; i < kNumTasks; ++i) {
      auto node = folly::to<std::string>("node", i);  // ID tasks by the node
      rt.node = node;
      tsq.runTask(
        rt,
        cmd,
        "json_arg",
        ".",
        [node](const cpp2::RunningTask& rt2, TaskStatus&& status) noexcept {
          EXPECT_EQ("job", rt2.job);
          EXPECT_EQ(node, rt2.node);
          EXPECT_TRUE(status.isDone());
        },
        [node, this](
          const cpp2::RunningTask& rt2, cpp2::TaskPhysicalResources&&
        ) {
          EXPECT_EQ("job", rt2.job);
          EXPECT_EQ(node, rt2.node);
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
  SYNCHRONIZED(task_to_logs) {
    EXPECT_EQ(kNumTasks, task_to_logs.size());
    for (int i = 0; i < kNumTasks; ++i) {
      auto node = folly::to<std::string>("node", i);
      auto full_cmd = cmd;
      // The base command should get 3 arguments appended to it:
      full_cmd.insert(full_cmd.end(), {
        node, folly::to<std::string>("/dev/fd/", pipe_fd), "json_arg"
      });
      checkNormalTaskLogs(task_to_logs[std::make_pair("job", node)], full_cmd);
    }
  }
}

TEST_F(TestTaskSubprocessQueue, NoStatus) {
  {
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>());
    cpp2::RunningTask rt;
    rt.job = "job";
    rt.node = "node";
    tsq.runTask(
      rt,
      std::vector<std::string>{"/bin/echo"},
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt2, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
        EXPECT_EQ(
          TaskStatusBits::Error | TaskStatusBits::UsesBackoff,
          status.bits()
        );
        EXPECT_EQ(
          "Failed to read a status",
          (*status.dataThreadUnsafe()).at("exception").asString()
        );
      },
      [this](const cpp2::RunningTask& rt2, cpp2::TaskPhysicalResources&&) {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
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
  // A simple "sleep 1" gets `exec()`ed by some `sh`s
  runAndKill("sleep 1 ; sleep 3600", opts);
  EXPECT_LE(1.0, timeSince(startTime_));
}

TEST_F(TestTaskSubprocessQueue, ProcGroupKillsChild) {
  // Making a process group lets us kill both `sh` and `sleep` quickly.
  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = true;
  // A simple "sleep 1" gets `exec()`ed by some `sh`s
  runAndKill("sleep 3600 ; sleep 3600", opts);
  EXPECT_GT(1.0, timeSince(startTime_));
}

TEST_F(TestTaskSubprocessQueue, FailsToStart) {
  {
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>());
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
          (*status.dataThreadUnsafe()).at("exception").asString()
        );
      },
      [this](const cpp2::RunningTask&, cpp2::TaskPhysicalResources&&) {},
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
  folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;
  // The SIGTERM 'trap' callback will be SIGKILLed while it sleeps.
  // Sleep 2 seconds, or 2 seconds after TERM
  cpp2::KillRequest req;
  req.method = cpp2::KillMethod::TERM_WAIT_KILL;
  req.killWaitMs = 1000;  // SIGKILL in 1 second
  runAndKillWithStatus(req, 2, 2, &task_to_logs);
  EXPECT_LE(2.0, timeSince(startTime_));  // The original 2-second sleep runs.

  // Validate the event log and capture soft-kill timings
  SYNCHRONIZED(task_to_logs) {
    TestTimePoint process_exit_time, pipes_closed_time;
    EXPECT_EQ(1, task_to_logs.size());
    auto& logs = task_to_logs[std::make_pair("job", "node")];
    EXPECT_EQ(0, logs.stdout_.size());
    EXPECT_EQ(0, logs.stderr_.size());
    size_t event_idx = 0;
    for (auto& expected_event : std::vector<dynamic>{
      dynamic::object
        ("event", "running")
        // I'm too lazy to check the command's contents.
        ("command", dynamic::array()),
      dynamic::object
        ("event", "process_exited")
        ("raw_status", "done")
        // Since the task produced its own status, Bistro treats it as if it
        // succeded -- this message is the only evidence of the SIGKILL.
        ("message", "killed by signal 9"),
      dynamic::object
        ("event", "task_pipes_closed")
        ("raw_status", "done"),
      dynamic::object
        ("event", "got_status")
        ("raw_status", "done")
        ("status", dynamic::object("result_bits", 4)),
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
    EXPECT_NE(TestTimePoint(), process_exit_time);  // initialized
    EXPECT_NE(TestTimePoint(), pipes_closed_time);  // initialized
    // The SIGKILL happens just over 1 sec after start
    EXPECT_LE(1.0, timeDiff(process_exit_time, startTime_));
    // The `sleep` child process waits the full 2 sec, and holds the pipe open.
    EXPECT_LE(2.0, timeDiff(pipes_closed_time, startTime_));
    // There should be a good gap between the SIGKILL and the sleep's exit.
    EXPECT_LE(0.5, timeDiff(pipes_closed_time, process_exit_time));
  }
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
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(
      /*task_to_logs=*/ nullptr, /*print_logs=*/ false
    ));
    tsq.runTask(
      rt,
      cmd,
      "json_arg",
      ".",
      [](const cpp2::RunningTask& rt2, TaskStatus&& status) noexcept {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
        EXPECT_TRUE(status.isDone());
      },
      [this](const cpp2::RunningTask& rt2, cpp2::TaskPhysicalResources&&) {
        EXPECT_EQ("job", rt2.job);
        EXPECT_EQ("node", rt2.node);
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

std::string cgDir(std::string subsystem) {
  return folly::to<std::string>(  // startTime & rand are 0 (The Epoch) & 0
    "root/", subsystem, "/slice/shard:19700101000000:0:", getpid()
  );
};

// Basic integration test -- test_cgroup_setup.cpp has deeper coverage
TEST_F(TestTaskSubprocessQueue, AddToCGroups) {
  folly::test::ChangeToTempDir td;
  namespace p = std::placeholders;  // p::_1 to avoid conflicting with boost

  // Nothing happens until we specify some subsystems.
  cpp2::TaskSubprocessOptions opts;
  opts.cgroupOptions.unitTestCreateFiles = true;
  opts.cgroupOptions.root = "root";
  opts.cgroupOptions.slice = "slice";
  {
    // tsq's destructor is the easiest way to await task exit :)
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>());
    runTask(&tsq, "exec sleep 3600", opts, &expectKilled);
    SCOPE_EXIT { tsq.kill(runningTask(), requestSigkill()); };
    EXPECT_TRUE(boost::filesystem::is_empty("."));
  }

  // The slice directory must exist for this subsystem.
  opts.cgroupOptions.subsystems = {"sys"};
  {
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>());
    runTask(&tsq, "exec sleep 3600", opts, std::bind(
      &expectErrorRegex, ".*root/subsystem/slice must be a dir.*", p::_1, p::_2
    ));
  }

  // Once we make the slice directory, everything works.
  EXPECT_TRUE(boost::filesystem::create_directories("root/sys/slice"));
  {
    folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(&task_to_logs));
    runTask(&tsq, "/bin/echo ; exec sleep 3600", opts, &expectKilled);
    SCOPE_EXIT { tsq.kill(runningTask(), requestSigkill()); };
    while (  // Wait for the task to start.
      task_to_logs->operator[](std::make_pair("job", "node")).stdout_.empty()
    ) {
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Validate the contents of the cgroup directory.
    auto check_file_fn = [&](std::string filename, std::string expected) {
      std::string s;
      auto path = cgDir("sys") + "/" + filename;
      EXPECT_TRUE(folly::readFile(path.c_str(), s));
      EXPECT_EQ(expected, s) << " in " << path;
      EXPECT_TRUE(boost::filesystem::remove(path));  // For is_empty(directory)
    };
    check_file_fn("cgroup.procs", "0");  // "0" means "add my PID".
    check_file_fn("notify_on_release", "1");
    EXPECT_TRUE(boost::filesystem::is_empty(cgDir("sys")));  // No other files
  }
}

void waitForEvent(
    folly::Synchronized<TestLogWriter::TaskLogMap>& task_to_logs,
    std::string awaited_event) {
  while (true) {
    SYNCHRONIZED(task_to_logs) {
      for (const auto& event
           : task_to_logs[std::make_pair("job", "node")].events_) {
        if (event["event"].asString() == awaited_event) {
          return;
        }
      }
    }
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Without "freezer", we will just wait for the unkillable 'sleep'.
TEST_F(TestTaskSubprocessQueue, AsyncCGroupReaperNoFreezer) {
  folly::test::ChangeToTempDir td;

  // Without "freezer", we will just wait for the unkillable 'sleep'.
  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = false;  // Or `sleep`'d be killed without cgroups
  opts.cgroupOptions.unitTestCreateFiles = true;
  opts.cgroupOptions.root = "root";
  opts.cgroupOptions.slice = "slice";
  opts.cgroupOptions.subsystems = {"cpu"};
  EXPECT_TRUE(boost::filesystem::create_directories("root/cpu/slice"));
  {
    folly::test::CaptureFD stderr(2, printString);
    // This scope uses `tsq`'s destructor to await task exit.
    folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(&task_to_logs));
    // We can kill the shell, but not the `sleep`. The trailing `echo`
    // ensures that `sh` does not just `exec()` the `sleep`.
    runTask(&tsq, "/bin/sleep 1 ; /bin/echo DONE", opts, &expectKilled);

    // fork + exec + kill should take << 1 sec
    tsq.kill(runningTask(), requestSigkill());
    waitForEvent(task_to_logs, "process_exited");
    EXPECT_GT(1.0, timeSince(startTime_));

    // Now we are waiting for "sleep" to exit, which must take >= 1 sec.
    waitForEvent(task_to_logs, "task_pipes_closed");
    EXPECT_LE(1.0, timeSince(startTime_));

    // Confirm that the reaper is following an exponential backoff schedule.
    auto trying_to_reap_re_fn = [](int ms) { return folly::to<std::string>(
      "W[^\n]*] ",  // glog warning boilerplate
      "Trying to reap intransigent task with cgroup shard:19700101000000:0:",
      getpid(), " for over ", ms, " ms\n"
    ); };
    auto not_sending_re = folly::to<std::string>(
      "W[^\n]*] ",  // glog warning boilerplate
      "Not sending SIGKILL to tasks in the shard:19700101000000:0:", getpid(),
      " cgroups, since the `freezer` subsystem is not enabled, which would ",
      "make it easy to kill the wrong processes.\n"
    );
    std::string lines_re = "(.*\n)*";  // Match 0 or more whole lines.
    waitForRegexOnFd(
      &stderr,
      ".*\n" + not_sending_re + lines_re  // tries to signal in constructor
        + trying_to_reap_re_fn(10) + not_sending_re + lines_re
        + trying_to_reap_re_fn(50) + not_sending_re + lines_re
        + trying_to_reap_re_fn(210) + not_sending_re + lines_re
        + trying_to_reap_re_fn(850) + not_sending_re + ".*"
    );
    stderr.release();

    // At this point, the task is blocked only on the cgroup reaper,
    // so change the cgroup to look like all PIDs have exited.
    auto cpu_procs_path = cgDir("cpu") + "/cgroup.procs";
    EXPECT_TRUE(folly::writeFile(std::string(), cpu_procs_path.c_str()));

    // Thanks to exponential backoff, this wait is ~4x longer than the last.
    waitForEvent(task_to_logs, "cgroups_reaped");
    EXPECT_LE(3.41, timeSince(startTime_));
    EXPECT_GT(13, timeSince(startTime_));  // But we didn't wait **too** long.
  }
}

std::string waitForFirstStdout(
    folly::Synchronized<TestLogWriter::TaskLogMap>* task_to_logs) {
  while (true) {
    SYNCHRONIZED(task_to_logs, *task_to_logs) {
      auto stdout = task_to_logs[std::make_pair("job", "node")].stdout_;
      if (!stdout.empty()) {
        return stdout.back();  // Newline and all
      }
    }
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return "not a pid";  // Never reached
}

// Unlike AsyncCGroupReaperNoFreezer, enabling "freezer" lets us actually
// kill the "sleep" descendant instead of waiting for the cgroup.
TEST_F(TestTaskSubprocessQueue, AsyncCGroupReaperWithFreezer) {
  folly::test::ChangeToTempDir td;

  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = false;  // Or `sleep`'d be killed without cgroups
  opts.cgroupOptions.unitTestCreateFiles = true;
  opts.cgroupOptions.root = "root";
  opts.cgroupOptions.slice = "slice";
  opts.cgroupOptions.subsystems = {"cpu", "freezer"};
  EXPECT_TRUE(boost::filesystem::create_directories("root/cpu/slice"));
  EXPECT_TRUE(boost::filesystem::create_directories("root/freezer/slice"));
  {
    // This scope uses `tsq`'s destructor to await task exit.
    folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(&task_to_logs));
    // Since processGroupLeader is off, the initial kill only affect the
    // outer shell, but not the `sleep`.  The inner shell is an easy way to
    // find the sleep's PID.  The trailing `echo` ensures that `sh` does not
    // just `exec()` the `sleep`.
    runTask(
      &tsq, "/bin/sh -c '/bin/echo $$ ; exec /bin/sleep 9999' ; /bin/echo",
      opts, &expectKilled
    );

    // Add the `sleep`'s PID to the cgroup **before** initiating the kill,
    // otherwise we'll be signaling a PID of 0, i.e. ourselves.
    auto pid_str = waitForFirstStdout(&task_to_logs);
    auto freezer_procs_path = cgDir("freezer") + "/cgroup.procs";
    ASSERT_TRUE(folly::writeFile(pid_str, freezer_procs_path.c_str()));

    // Also have to initialize freezer.state, as a real cgroup would.
    auto freezer_state_path = cgDir("freezer") + "/freezer.state";
    auto freezer_state = std::string("THAWED\n");
    EXPECT_TRUE(folly::writeFile(freezer_state, freezer_state_path.c_str()));

    // fork + exec + kill should take << 1 sec
    tsq.kill(runningTask(), requestSigkill());
    waitForEvent(task_to_logs, "process_exited");
    EXPECT_GT(1.0, timeSince(startTime_));

    // Wait for the reaper to append FROZEN to the freezer state, and react.
    while (true) {
      EXPECT_TRUE(folly::readFile(freezer_state_path.c_str(), freezer_state));
      if (freezer_state == "THAWED\nFROZEN") {
        break;
      }
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    freezer_state = "FROZEN\n";
    EXPECT_TRUE(folly::writeFile(freezer_state, freezer_state_path.c_str()));
    // We need no more freezer.state updates to kill `sleep`, so leave it be.

    folly::test::CaptureFD stderr(2, printString);

    // Once the reaper kills `sleep`, update the `freezer` cgroup.
    waitForEvent(task_to_logs, "task_pipes_closed");
    EXPECT_GT(1.0, timeSince(startTime_));  // Still need << 1 sec
    EXPECT_TRUE(folly::writeFile(std::string(), freezer_procs_path.c_str()));

    // "cgroups_reaped" cannot have arrived yet, since "cpu"'s procs are
    // nonempty. Check this.
    SYNCHRONIZED(task_to_logs) {
      for (const auto& ev
           : task_to_logs[std::make_pair("job", "node")].events_) {
        EXPECT_NE("cgroups_reaped", ev.at("event").asString());
      }
    }

    // Now, empty the "cpu" cgroup, and
    auto cpu_procs_path = cgDir("cpu") + "/cgroup.procs";
    ASSERT_TRUE(folly::writeFile(std::string(), cpu_procs_path.c_str()));
    waitForEvent(task_to_logs, "cgroups_reaped");
    EXPECT_GT(1, timeSince(startTime_));  // Still need << 1 sec

    // Since these aren't real cgroups, the directories cannot be removed,
    // but the logs show evidence that we tried.
    EXPECT_PCRE_MATCH(folly::to<std::string>(
      "(^|.*\n)W[^\n]*] Failed to remove empty cgroup: ", cgDir("cpu"),
      ": Directory not empty",
      "\nW[^\n]*] Failed to remove empty cgroup: ", cgDir("freezer"),
      ": Directory not empty\n.*"
    ), stderr.readIncremental());
  }
  EXPECT_GT(1, timeSince(startTime_));  // start-to-finish < 1 sec
}

// Signal without freezer, incurring the risk of killing the wrong process.
TEST_F(TestTaskSubprocessQueue, AsyncCGroupReaperKillWithoutFreezer) {
  folly::test::ChangeToTempDir td;

  cpp2::TaskSubprocessOptions opts;
  opts.processGroupLeader = false;  // Or `sleep`'d be killed without cgroups
  opts.cgroupOptions.unitTestCreateFiles = true;
  opts.cgroupOptions.root = "root";
  opts.cgroupOptions.slice = "slice";
  opts.cgroupOptions.subsystems = {"cpu"};
  opts.cgroupOptions.killWithoutFreezer = true;
  EXPECT_TRUE(boost::filesystem::create_directories("root/cpu/slice"));
  {
    // This scope uses `tsq`'s destructor to await task exit.
    folly::Synchronized<TestLogWriter::TaskLogMap> task_to_logs;
    TaskSubprocessQueue tsq(std::make_unique<TestLogWriter>(&task_to_logs));
    // Since processGroupLeader is off, the initial kill only affect the
    // outer shell, but not the `sleep`.  The inner shell is an easy way to
    // find the sleep's PID.  The trailing `echo` ensures that `sh` does not
    // just `exec()` the `sleep`.
    runTask(
      &tsq, "/bin/sh -c '/bin/echo $$ ; exec /bin/sleep 9999' ; /bin/echo",
      opts, &expectKilled
    );

    // Add the `sleep`'s PID to our only cgroup **before** initiating the
    // kill, otherwise we'll be signaling a PID of 0, i.e. ourselves.
    auto pid_str = waitForFirstStdout(&task_to_logs);
    auto cpu_procs_path = cgDir("cpu") + "/cgroup.procs";
    ASSERT_TRUE(folly::writeFile(pid_str, cpu_procs_path.c_str()));

    // fork + exec + kill should take << 1 sec
    tsq.kill(runningTask(), requestSigkill());
    waitForEvent(task_to_logs, "process_exited");
    EXPECT_GT(1.0, timeSince(startTime_));

    // Wait for the reaper to kill `sleep`.
    waitForEvent(task_to_logs, "task_pipes_closed");
    EXPECT_GT(1.0, timeSince(startTime_));  // Still need << 1 sec

    // "cgroups_reaped" cannot have arrived yet, since "cpu"'s procs are
    // nonempty. Check this.
    SYNCHRONIZED(task_to_logs) {
      for (const auto& ev
           : task_to_logs[std::make_pair("job", "node")].events_) {
        EXPECT_NE("cgroups_reaped", ev.at("event").asString());
      }
    }

    // Update the `cpu` cgroup so that the reaper can exit.
    EXPECT_TRUE(folly::writeFile(std::string(), cpu_procs_path.c_str()));
    waitForEvent(task_to_logs, "cgroups_reaped");
    EXPECT_GT(1, timeSince(startTime_));  // Still need << 1 sec
  }
  EXPECT_GT(1, timeSince(startTime_));  // start-to-finish < 1 sec
}
