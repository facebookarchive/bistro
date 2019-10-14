/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/processes/SubprocessOutputWithTimeout.h"

using namespace facebook::bistro;

TEST(TestSubprocessOutputWithTimeout, WaitForSleep) {
  std::vector<std::string> cmd{"/bin/sleep", "4"}, stdOut, stdErr;
  auto retCode = subprocessOutputWithTimeout(cmd, &stdOut, &stdErr, 8000);
  EXPECT_TRUE(retCode.exited());
  EXPECT_EQ(retCode.exitStatus(), 0);
  EXPECT_EQ(stdOut.size(), 0);
  EXPECT_EQ(stdErr.size(), 0);
}

TEST(TestSubprocessOutputWithTimeout, TimedOutSleep) {
  std::vector<std::string> cmd{"/bin/sleep", "4"}, stdOut, stdErr;
  auto retCode = subprocessOutputWithTimeout(cmd, &stdOut, &stdErr, 1000);
  EXPECT_TRUE(retCode.killed());
  EXPECT_EQ(retCode.killSignal(), SIGKILL);
  EXPECT_EQ(stdOut.size(), 0);
  EXPECT_EQ(stdErr.size(), 0);
}

TEST(TestSubprocessOutputWithTimeout, HelpOutput) {
  std::vector<std::string> cmd{"/bin/sleep", "--help"}, stdOut, stdErr;
  auto retCode = subprocessOutputWithTimeout(cmd, &stdOut, &stdErr, 1000);
  EXPECT_TRUE(retCode.exited());
  EXPECT_EQ(retCode.exitStatus(), 0);
  EXPECT_GE(stdOut.size(), 1);
  EXPECT_EQ(stdErr.size(), 0);
}

TEST(TestSubprocessOutputWithTimeout, HelpOutputNullBuffers) {
  std::vector<std::string> cmd{"/bin/sleep", "--help"};
  auto retCode = subprocessOutputWithTimeout(cmd, nullptr, nullptr, 1000);
  EXPECT_TRUE(retCode.exited());
  EXPECT_EQ(retCode.exitStatus(), 0);
}

TEST(TestSubprocessOutputWithTimeout, WrongBinary) {
  std::vector<std::string> cmd{"/bin/sleepNotExists"}, stdOut, stdErr;
  auto retCode = subprocessOutputWithTimeout(cmd, &stdOut, &stdErr, 1000);
  EXPECT_TRUE(retCode.notStarted());
  EXPECT_EQ(stdOut.size(), 0);
  EXPECT_EQ(stdErr.size(), 0);
}

TEST(TestSubprocessOutputWithTimeout, WrongParameters) {
  std::vector<std::string> cmd{"/bin/sleep", "notInt"}, stdOut, stdErr;
  auto retCode = subprocessOutputWithTimeout(cmd, &stdOut, &stdErr, 1000);
  EXPECT_TRUE(retCode.exited());
  EXPECT_EQ(retCode.exitStatus(), 1);
  EXPECT_EQ(stdOut.size(), 0);
  EXPECT_GE(stdErr.size(), 1);
}

TEST(TestSubprocessOutputWithTimeout, ParentGroupKill) {
  std::vector<std::string> cmd{"/bin/sh", "-c", "sleep 3600 ; sleep 3600"},
    stdOut, stdErr;
  auto retCode = subprocessOutputWithTimeout(cmd, &stdOut, &stdErr, 1000);
  EXPECT_TRUE(retCode.killed());
  EXPECT_EQ(retCode.killSignal(), SIGKILL);
  EXPECT_EQ(stdOut.size(), 0);
  EXPECT_EQ(stdErr.size(), 0);
}
