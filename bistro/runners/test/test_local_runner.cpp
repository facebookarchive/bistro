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

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/runners/LocalRunner.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/TemporaryFile.h"
#include <folly/Synchronized.h>

using namespace facebook::bistro;
using namespace folly;
using namespace std;

DECLARE_int32(log_prune_frequency);

const Config kConfig(dynamic::object
  ("enabled", true)
  ("nodes", dynamic::object
    ("levels", { "level1" , "level2" })
    ("node_source", "range_label")
    ("node_source_prefs", dynamic::object)
  )
  ("resources", dynamic::object)
);
const dynamic kJob = dynamic::object
  ("enabled", true)
  ("owner", "owner")
  ("backoff", {"fail"})  // Test the weird "no backoff" default backoff.
;

TEST(TestLocalRunner, HandleAll) {
  FLAGS_log_prune_frequency = 0;

  TemporaryDir tmp_dir;
  TemporaryFile cmdFile(tmp_dir.createFile());
  cmdFile.writeString(
    "#!/bin/sh\n"
    "echo -n \"this is my stdout\"\n"
    "echo -n \"this is my stderr\" 1>&2\n"
    "echo \"done\" >$2"
  );
  PCHECK(chmod(cmdFile.getFilename().c_str(), 0700) == 0);
  LocalRunner runner(cmdFile.getFilename(), tmp_dir.getPath());

  auto job = make_shared<Job>(kConfig, "foo_job", kJob);
  auto node = make_shared<Node>("test_node");

  auto start_time = time(nullptr);
  Synchronized<TaskStatus> status;
  runner.runTask(
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
    this_thread::sleep_for(chrono::milliseconds(100));
  }
  ASSERT_TRUE(status->isDone());

  for (const auto& logtype : vector<string>{"stdout", "stderr", "statuses"}) {
    // A very basic log test of log retrieval, does not check most features.
    auto log = runner.getJobLogs(
      logtype,
      vector<string>{job->name()},
      vector<string>{node->name()},
      0,  // line_id
      true,  // is_ascending
      ".*.*"  // a match-all regex filter
    );
    if (logtype != "statuses") {
      ASSERT_EQ(1, log.lines.size());
      ASSERT_EQ("this is my " + logtype, log.lines.back().line);
    } else {
      ASSERT_EQ(3, log.lines.size());
      ASSERT_LE(start_time, log.lines[0].time);
      ASSERT_EQ(job->name(), log.lines[0].jobID);
      ASSERT_EQ(node->name(), log.lines[0].nodeID);
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
    ASSERT_EQ(job->name(), log.lines.back().jobID);
    ASSERT_EQ(node->name(), log.lines.back().nodeID);
    ASSERT_EQ(LogLine::kNotALineID, log.nextLineID);
  }
}

TEST(TestLocalRunner, HandleKill) {
  FLAGS_log_prune_frequency = 0;

  TemporaryDir tmp_dir;
  TemporaryFile cmdFile(tmp_dir.createFile());
  cmdFile.writeString(
    "#!/bin/sh\n"
    "sleep 100\n"
    "echo \"done\" >$2"
  );
  PCHECK(chmod(cmdFile.getFilename().c_str(), 0700) == 0);
  LocalRunner runner(cmdFile.getFilename(), tmp_dir.getPath());

  auto job = make_shared<Job>(kConfig, "job", kJob);
  const size_t kNumNodes = 5;
  Synchronized<std::vector<TaskStatus>> status_seqs[kNumNodes];

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
    auto node = make_shared<Node>(to<string>("node", i));
    runner.runTask(
      kConfig,
      job,
      node,
      nullptr,  // no previous status
      [&status_seqs, i](const cpp2::RunningTask& rt, TaskStatus&& st) {
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

  const auto assertTaskKilledNoFilter = [&](int n) {
    ASSERT_EQ(2, status_seqs[n]->size());
    assertTaskOnNode(n, 1, "incomplete_backoff", [](const TaskStatus& status) {
      return status.bits() == kIncompleteBackoffBits
        && status.backoffDuration().seconds == 60 // See JobBackoffSettings.cpp
        && status.data()
        && status.data()->at("exception") == "Task killed, no status returned";
    });
  };

  runner.killTask("job", "node0", cpp2::KilledTaskStatusFilter::NONE);
  assertTaskKilledNoFilter(0);
  assertTasksRunningFromNode(1);  // Other tasks are unaffected.

  runner.killTask(
    "job", "node1",
    cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE_BACKOFF
  );
  ASSERT_EQ(2, status_seqs[1]->size());
  assertTaskOnNode(1, 1, "incomplete_backoff", [](const TaskStatus& status) {
    return status.bits() == kIncompleteBackoffBits
      && status.backoffDuration().seconds == 60  // See JobBackoffSettings.cpp
      && status.data()
      && status.data()->count("actual_status")
      && !status.data()->count("exception")
      && status.data()->at("message")
        == "Killed & coerced to 'incomplete_backoff' since task was not done";
  });
  assertTasksRunningFromNode(2);  // Other tasks are unaffected.

  runner.killTask(
    "job", "node2", cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE
  );
  // Even though it's "incomplete", it will not respawn since we aren't
  // running a real scheduling loop here.
  ASSERT_EQ(2, status_seqs[2]->size());
  assertTaskOnNode(2, 1, "incomplete", [](const TaskStatus& status) {
    return status.bits() == TaskStatusBits::Incomplete;
  });
  assertTasksRunningFromNode(3);  // Other tasks are unaffected.

  // All remaining running tasks get killed
  for (size_t i = 3; i < kNumNodes; ++i) {
    runner.killTask(
      "job", folly::to<std::string>("node", i),
      cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_FAILED
    );
    ASSERT_EQ(2, status_seqs[i]->size());
    assertTaskOnNode(i, 1, "failed", [](const TaskStatus& s) {
      return s.isFailed();
    });
  }

  // But a task that wasn't running remains in backoff. Note: Since it may
  // run again, this is probably not what the user wants.  We should make
  // this better?
  assertTaskKilledNoFilter(0);
}
