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

#include <folly/Conv.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <sys/file.h>
#include <thread>

#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/ProcessRunner.h"

using namespace facebook::bistro;
using namespace folly;
using namespace std;

const string kNormal =
  "#!/bin/sh\n"
  "echo \"this is my stdout\"\n"
  "echo $1 $3 $4\n"
  "echo -n \"this is my stderr\" 1>&2\n"
  "echo \"done\" >$2";

const string kError =
  "#!/bin/sh\n"
  "exit 1";

const dynamic kRunningStatus = dynamic::object
  ("result", "running")
  ("data", dynamic::object("worker_host", getLocalHostName()));

struct TestRunner {
  template<typename... Args>
  explicit TestRunner(
      const string& content,
      unique_ptr<TemporaryFile>&& pipe_file,
      vector<string>&& args)
    : tmpDir("/tmp"),
      cmdFile([this, content]() {
        // Set up the command file *before* starting the process
        auto f = tmpDir.createFile();
        f.writeString(content);
        PCHECK(chmod(f.getFilename().c_str(), 0700) == 0);
        return f;
      }()),
      runner(
        [&](LogTable lt, folly::StringPiece s) {
          if (lt == LogTable::STDOUT) {
            stdout.push_back(s.str());
          } else if (lt == LogTable::STDERR) {
            stderr.push_back(s.str());
          } else if (lt == LogTable::STATUSES) {
            statuses.push_back(s.str());
          } else {
            throw BistroException("Unknown log type ", lt);
          }
        },
        "/tmp",  // working directory
        {cmdFile.getFilename().native()},
        make_pair(args, std::move(pipe_file))
      ) {}
  TemporaryDir tmpDir;
  TemporaryFile cmdFile;
  // Must be initialized before starting the process
  vector<string> stdout, stderr, statuses;
  ProcessRunner runner;
};

TEST(TestProcessRunner, HandleWaitPipe) {
  auto pipe_file = make_unique<TemporaryFile>("/tmp");
  auto pipe_filename = pipe_file->getFilename().native();
  TestRunner t(kNormal, std::move(pipe_file), vector<string>{
    "arg1", pipe_filename, "arg2", "77"
  });
  string result = t.runner.wait().value();
  ASSERT_EQ("done", result);
  ASSERT_EQ(2, t.stdout.size());
  ASSERT_EQ("this is my stdout\n", t.stdout[0]);
  ASSERT_EQ("arg1 arg2 77\n", t.stdout[1]);
  ASSERT_EQ(1, t.stderr.size());
  ASSERT_EQ("this is my stderr", t.stderr[0]);
  ASSERT_EQ(3, t.statuses.size());
  EXPECT_EQ(kRunningStatus, parseJson(t.statuses[0]));
  ASSERT_EQ("exited", t.statuses[1]);
  ASSERT_EQ("done", t.statuses[2]);
}

TEST(TestProcessRunner, KillFromOtherThread) {
  // TODO: When I add support for process group leaders, remove the "exec"
  auto start = std::chrono::high_resolution_clock::now();
  // Will be killed after 100ms
  TestRunner t("#!/bin/sh\nexec sleep 60", nullptr, {});

  // Kill the child after running it for a bit
  thread kill_thread([&t](){
    this_thread::sleep_for(chrono::milliseconds(100));
    t.runner.softKill();
  });
  SCOPE_EXIT { kill_thread.join(); };

  EXPECT_FALSE(t.runner.wait().hasValue());
  double duration = std::chrono::duration_cast<std::chrono::duration<double>>(
    std::chrono::high_resolution_clock::now() - start
  ).count();
  EXPECT_GE(1.0, duration);  // Make sure it didn't run for a minute
  ASSERT_EQ(3, t.statuses.size());
  EXPECT_EQ(kRunningStatus, parseJson(t.statuses[0]));
  ASSERT_EQ("soft-killed", t.statuses[1]);
  ASSERT_EQ("", t.statuses[2]);
}

TEST(TestProcessRunner, HandleWaitNoPipe) {
  TemporaryFile dummy("/tmp");
  TestRunner t(kNormal, nullptr, vector<string>{
    "arg1", dummy.getFilename().native(), "arg2", "77"
  });
  string result = t.runner.wait().value();
  ASSERT_TRUE(result.empty());
  ASSERT_EQ(2, t.stdout.size());
  ASSERT_EQ("this is my stdout\n", t.stdout[0]);
  ASSERT_EQ("arg1 arg2 77\n", t.stdout[1]);
  ASSERT_EQ(1, t.stderr.size());
  ASSERT_EQ("this is my stderr", t.stderr[0]);
  ASSERT_EQ(3, t.statuses.size());
  EXPECT_EQ(kRunningStatus, parseJson(t.statuses[0]));
  ASSERT_EQ("exited", t.statuses[1]);
  ASSERT_EQ("", t.statuses[2]);
}
