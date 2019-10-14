/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/Synchronized.h>
#include <thread>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/runners/LocalRunner.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/TemporaryFile.h"

using namespace facebook::bistro;
using folly::dynamic;

DECLARE_int32(log_prune_frequency);

const Config kConfig(dynamic::object
  ("enabled", true)
  ("nodes", dynamic::object
    ("levels", dynamic::array("level1" , "level2"))
    ("node_sources", dynamic::array(dynamic::object
      ("source", "range_label")
      ("prefs", dynamic::object)
    ))
  )
  ("resources", dynamic::object)
  // Need to make leaders for the signal to reach the `sleep` child process.
  ("task_subprocess", dynamic::object("process_group_leader", true))
);
const dynamic kJob = dynamic::object
  ("enabled", true)
  ("owner", "owner")
  // Test the weird "no backoff" default backoff.
  ("backoff", dynamic::array("fail"))
;

struct TestLocalRunner : public ::testing::Test {
  TestLocalRunner() : cmdFile_(tmpDir_.createFile()) {
    FLAGS_log_prune_frequency = 0;
  }
  TemporaryDir tmpDir_;
  TemporaryFile cmdFile_;
};

void checkLogsEq(const std::vector<std::string>& lines, const LogLines& log) {
  ASSERT_EQ(lines.size(), log.lines.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    ASSERT_EQ(lines[i], log.lines[i].line);
  }
}

void checkDoneTaskAndLogs(
    LocalRunner* runner,
    const folly::dynamic& d_job,
    std::vector<std::string> stdout,
    std::vector<std::string> stderr) {

  auto job = std::make_shared<Job>(kConfig, "foo_job", d_job);
  Node node("test_node");

  auto start_time = time(nullptr);
  folly::Synchronized<TaskStatus> status;
  runner->runTask(
    kConfig,
    job,
    node,
    nullptr,  // no previous status
    [&status](const cpp2::RunningTask& rt, TaskStatus&& st) {
      status->update(rt, std::move(st));
    }
  );
  ASSERT_TRUE(status->isRunning());

  while (status->isRunning()) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(status->isDone());

  for (const auto& logtype : std::vector<std::string>{
    "stdout", "stderr", "statuses"
  }) {
    // A very basic log test of log retrieval, does not check most features.
    auto log = runner->getJobLogs(
      logtype,
      std::vector<std::string>{job->name()},
      std::vector<std::string>{node.name()},
      0,  // line_id
      true,  // is_ascending
      ".*.*"  // a match-all regex filter
    );
    if (logtype == "stdout") {
      checkLogsEq(stdout, log);
    } else if (logtype == "stderr") {
      checkLogsEq(stderr, log);
    } else if (logtype == "statuses") {
      ASSERT_EQ(4, log.lines.size());
      ASSERT_LE(start_time, log.lines[0].time);
      ASSERT_EQ(job->name(), log.lines[0].jobID);
      ASSERT_EQ(node.name(), log.lines[0].nodeID);
      EXPECT_EQ(
        "running", folly::parseJson(log.lines[0].line)["event"].asString()
      );
      int proc_exit_idx = 2;
      if (folly::parseJson(log.lines[1].line)["event"]
          != "task_pipes_closed") {
        proc_exit_idx = 1;
        ASSERT_EQ(
          "task_pipes_closed",
          folly::parseJson(log.lines[2].line)["event"].asString()
        );
      }
      ASSERT_EQ(
        "process_exited",
        folly::parseJson(log.lines[proc_exit_idx].line)["event"].asString()
      );
      auto j = folly::parseJson(log.lines[3].line);
      ASSERT_EQ("got_status", j["event"].asString());
      ASSERT_EQ("done", j["raw_status"].asString());
    } else {
      FAIL() << "Unknown log type: " << logtype;
    }
    ASSERT_LE(start_time, log.lines.back().time);
    ASSERT_EQ(job->name(), log.lines.back().jobID);
    ASSERT_EQ(node.name(), log.lines.back().nodeID);
    ASSERT_EQ(LogLine::kNotALineID, log.nextLineID);
  }
}

TEST_F(TestLocalRunner, HandleDoneTaskAndLogs) {
  cmdFile_.writeString(
    "#!/bin/sh\n"
    "echo -n \"this is my stdout\"\n"
    "echo -n \"this is my stderr\" 1>&2\n"
    "echo \"done\" > $2"
  );
  PCHECK(chmod(cmdFile_.getFilename().c_str(), 0700) == 0);
  LocalRunner runner(cmdFile_.getFilename(), tmpDir_.getPath());
  checkDoneTaskAndLogs(
    &runner, kJob, {"this is my stdout"}, {"this is my stderr"}
  );
}

TEST_F(TestLocalRunner, CustomCommand) {
  cmdFile_.writeString(
    "#!/bin/sh\n"
    "echo $1\n"
    "echo $2 1>&2\n"
    "echo \"done\" > $4"
  );
  PCHECK(chmod(cmdFile_.getFilename().c_str(), 0700) == 0);
  LocalRunner runner("/bad/worker_command", tmpDir_.getPath());
  auto d_job = kJob;
  d_job[kCommand] =
    dynamic::array(cmdFile_.getFilename().native(), "o", "e");
  checkDoneTaskAndLogs(&runner, d_job, {"o\n"}, {"e\n"});
}

TEST_F(TestLocalRunner, HandleKill) {
  cmdFile_.writeString(
    "#!/bin/sh\n"
    "sleep 10000\n"
    "echo \"done\" >$2"
  );
  PCHECK(chmod(cmdFile_.getFilename().c_str(), 0700) == 0);
  LocalRunner runner(cmdFile_.getFilename(), tmpDir_.getPath());

  auto job = std::make_shared<Job>(kConfig, "job", kJob);
  const size_t kNumNodes = 5;
  folly::Synchronized<std::vector<TaskStatus>> status_seqs[kNumNodes];
  folly::Synchronized<cpp2::RunningTask> rts[kNumNodes];

  auto assertTaskOnNode = [&](
      int node_num,
      size_t status_pos,
      const char* help,
      std::function<bool (const TaskStatus& status)> check_status_fn) {
    SYNCHRONIZED(status_seq, status_seqs[node_num]) {
      ASSERT_LT(status_pos, status_seq.size());
      ASSERT_TRUE(check_status_fn(status_seq[status_pos]))
        << "Expected task status #" << status_pos << " for node " << node_num
        << " to be '" << help << "', got " << status_seq[status_pos].toJson();
    }
  };

  auto assertTaskOnNodeIsRunning = [&](int node_num, size_t status_pos) {
    assertTaskOnNode(
      node_num, status_pos, "running",
      [](const TaskStatus& status) { return status.isRunning(); }
    );
  };

  for (size_t i = 0; i < kNumNodes; ++i) {
    Node node(folly::to<std::string>("node", i));
    runner.runTask(
      kConfig,
      job,
      node,
      nullptr,  // no previous status
      [&status_seqs, i, &rts](const cpp2::RunningTask& rt, TaskStatus&& st) {
        SYNCHRONIZED(last_rt, rts[i]) {
          last_rt = rt;
        }
        SYNCHRONIZED(status_seq, status_seqs[i]) {
          if (!status_seq.empty()) {
            // Copy and update lets us examine the whole status sequence.
            status_seq.emplace_back(status_seq.back());
          } else {
            status_seq.emplace_back();  // Default to an empty status
          }
          status_seq.back().update(rt, std::move(st));
        }
      }
    );
    ASSERT_EQ(1, status_seqs[i]->size());
    assertTaskOnNodeIsRunning(i, 0);
  }

  auto assertTasksRunningFromNode = [&](size_t min_node_num) {
    for (size_t i = min_node_num; i < kNumNodes; ++i) {
      ASSERT_EQ(1, status_seqs[i]->size());
      assertTaskOnNodeIsRunning(i, 0);
    }
  };

  const auto kIncompleteBackoffBits = TaskStatusBits::Incomplete
    | TaskStatusBits::UsesBackoff
    | TaskStatusBits::DoesNotAdvanceBackoff;

  // Kill all tasks.
  for (size_t i = 0; i < kNumNodes; ++i) {
    cpp2::RunningTask rt;
    SYNCHRONIZED(last_rt, rts[i]) {
      ASSERT_EQ("job", last_rt.job);  // Was it even set?
      rt = last_rt;
    }
    runner.killTask(rt, cpp2::KillRequest());
    // Wait for the task to die
    while (status_seqs[i]->size() < 2) {
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(2, status_seqs[i]->size());
    assertTaskOnNode(i, 1, "incomplete_backoff", [](const TaskStatus& status) {
      return status.bits() == kIncompleteBackoffBits
        // Third value from the default backoff in JobBackoffSettings.cpp
        && status.configuredBackoffDuration().seconds == 60
        && status.dataThreadUnsafe()
        && status.dataThreadUnsafe()->at("exception")
          == "Task killed, no status returned";
    });
    assertTasksRunningFromNode(i + 1);  // Other tasks are unaffected.
  }
}
