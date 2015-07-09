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

#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/Subprocess.h>
#include <thread>

#include "bistro/bistro/utils/KillableSubprocess.h"

using namespace facebook::bistro;
typedef KillableSubprocess::ExitReason ExitReason;

void noException(
    const folly::StringPiece debug_info,
    const folly::StringPiece msg,
    const std::exception& e) {

  LOG(FATAL) << "Unexpected exception: " << debug_info << ": " << msg << ": "
    << e.what();
}

std::string communicateStdout;
std::string communicateStderr;

void simpleCommunicate(std::shared_ptr<folly::Subprocess> subprocess) {
  auto pair = subprocess->communicate();
  communicateStdout = pair.first;
  communicateStderr = pair.second;
}

int exitStatus;
ExitReason exitReason;
std::string exitDebugInfo;

void recordExit(
    std::shared_ptr<folly::Subprocess>& subprocess,
    ExitReason exit_reason,
    const folly::StringPiece debug_info) {
  auto ret = subprocess->returnCode();
  exitStatus = ret.killed() ? -1 : ret.exitStatus();  // Easier with sentinel
  exitReason = exit_reason;
  exitDebugInfo = debug_info.toString();
}

void expectRun(
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time,
    double min_duration,
    double max_duration,
    int exit_status,
    ExitReason exit_reason,
    const std::string& debug_id,
    const std::string& stdout,
    const std::string& stderr) {

  double duration = std::chrono::duration_cast<std::chrono::duration<double>>(
    std::chrono::high_resolution_clock::now() - start_time
  ).count();
  EXPECT_LE(min_duration, duration);
  EXPECT_GE(max_duration, duration);
  EXPECT_EQ(exit_status, exitStatus);
  EXPECT_EQ(exit_reason, exitReason);
  EXPECT_PRED1([debug_id](const std::string& s){
    return s.find(debug_id) != std::string::npos;
  }, exitDebugInfo);
  EXPECT_EQ(stdout, communicateStdout);
  EXPECT_EQ(stderr, communicateStderr);
}

// Invoke one of these checks after the process has been reaped.

void checkKillNoOps(KillableSubprocess& p) {
  EXPECT_FALSE(p.softKill(10000));
  EXPECT_FALSE(p.hardKill());
}

void checkAllNoOps(KillableSubprocess& p) {
  EXPECT_FALSE(p.wait());
  checkKillNoOps(p);
}

TEST(TestKillableSubprocess, HandleWait) {
  const auto start_time = std::chrono::high_resolution_clock::now();
  KillableSubprocess p(folly::make_unique<folly::Subprocess>(
    "/bin/sleep 0.1", folly::Subprocess::pipeStdout().pipeStderr()
  ), simpleCommunicate, noException, recordExit, "my debug id");
  EXPECT_TRUE(p.wait());
  expectRun(start_time, 0.1, 0.2, 0, ExitReason::EXITED, "my debug id", "", "");
  checkAllNoOps(p);
}

void checkSimpleKill(
    const std::string& cmd,
    std::function<void(KillableSubprocess&)> kill_func,
    ExitReason exit_reason) {

  const auto start_time = std::chrono::high_resolution_clock::now();
  // Kill the child after running it for a bit
  KillableSubprocess p(folly::make_unique<folly::Subprocess>(
    cmd,
    folly::Subprocess::pipeStdout().pipeStderr()
  ), simpleCommunicate, noException, recordExit, "my debug id");

  std::thread kill_thread([&p, kill_func]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    kill_func(p);
    // wait() isn't reentrant, so checkAllNoOps is out.
    checkKillNoOps(p);
  });
  SCOPE_EXIT { kill_thread.join(); };

  EXPECT_FALSE(p.wait());  // blocks until kill_func reaps the process
  expectRun(start_time, 0.1, 0.2, -1, exit_reason, "my debug id", "", "");
  checkAllNoOps(p);
}

// For the below 4, I manually checked that the kills arrive in communicate()
// and in wait() respectively, by hacking wait() to say:
//
//    LOG(INFO) << "entering communicate";
//    communicate();
//    LOG(INFO) << "fds closed, starting to wait";
//    return pollAndSleep();
//
// And then checking that the kill order was as expected in the test logs.

TEST(TestKillableSubprocess, HandleSoftKillInCommunicate) {
  checkSimpleKill(
    "/bin/sleep 60",  // Would take a minute if the kill failed
    // 10s wait since "sleep" dies on SIGTERM
    [](KillableSubprocess& p) { p.softKill(10000); },
    ExitReason::SOFT_KILLED
  );
}

TEST(TestKillableSubprocess, HandleHardKillInCommunicate) {
  checkSimpleKill(
    "/bin/sleep 60",  // Would take a minute if the kill failed
    [](KillableSubprocess& p) { p.hardKill(); },
    ExitReason::HARD_KILLED
  );
}

TEST(TestKillableSubprocess, HandleSoftKillInWait) {
  checkSimpleKill(
    "exec 1>&-; exec 2>&-; /bin/sleep 60",  // Close FDs to leave communicate()
    // 10s wait since "sleep" dies on SIGTERM
    [](KillableSubprocess& p) { p.softKill(10000); },
    ExitReason::SOFT_KILLED
  );
}

TEST(TestKillableSubprocess, HandleHardKillInWait) {
  checkSimpleKill(
    "exec 1>&-; exec 2>&-; /bin/sleep 60",  // Close FDs to leave communicate()
    [](KillableSubprocess& p) { p.hardKill(); },
    ExitReason::HARD_KILLED
  );
}

TEST(TestKillableSubprocess, HandleTrappedSoftKill) {
  const auto start_time = std::chrono::high_resolution_clock::now();
  // Kill the child after running it for a bit
  KillableSubprocess p(folly::make_unique<folly::Subprocess>(
    // Exits in 0.1s after SIGTERM, or in 60s otherwise.
    "trap 'echo TERMed; sleep 0.15; kill $SLEEP_PID' TERM; "
    "/bin/sleep 60 & SLEEP_PID=$! ; wait",
    folly::Subprocess::pipeStdout().pipeStderr()
  ), simpleCommunicate, noException, recordExit, "my id");

  std::thread kill_thread([&p]() {
    // Wait a bit so the signal handler gets installed.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    p.softKill(10000);  // 10s wait since the handler quits in 0.15s
    checkKillNoOps(p);
  });
  SCOPE_EXIT { kill_thread.join(); };

  EXPECT_FALSE(p.wait());
  expectRun(
    start_time, 0.3, 0.4, 143, ExitReason::SOFT_KILLED, "my id", "TERMed\n", ""
  );
  checkAllNoOps(p);
}
