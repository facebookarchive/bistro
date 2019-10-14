/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/experimental/TestUtil.h>
#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <gtest/gtest.h>
#include <thread>

#include "bistro/bistro/physical/test/utils.h"

using namespace facebook::bistro;

struct TestSubprocessStats : public ::testing::Test {
  TestSubprocessStats()
   : kAppPid(777)
   , kAppMemory0(15461882265ULL)
   , kAppMemory1(25461882265ULL)
   , kAppCpuNsec0(1024*1024*1024*11ULL)
   , kAppCpuNsec1(1024*1024*1024*22ULL)
   , td()
   , kSlice("test.slice")
   , kTask("test")
   , subSystems({"cpuset", "cpuacct", "memory"})
   , cgroups(
       td.path().string(), kSlice, kTask, subSystems, td.path().string()
     )
   , resourceChecker(populateCGroups()) {}

  const uint64_t kAppPid;
  const uint64_t kAppMemory0;
  const uint64_t kAppMemory1;
  const uint64_t kAppCpuNsec0;
  const uint64_t kAppCpuNsec1;

  // -- temporary directory
  folly::test::ChangeToTempDir td;
  const std::string kSlice;
  const std::string kTask;
  const std::vector<std::string> subSystems;
  CGroupResources cgroups;

  boost::filesystem::path taskMemFile;
  boost::filesystem::path sliceCpuFile;
  boost::filesystem::path taskCpuFile;

  SubprocessResourceChecker resourceChecker;

  CGroupResources populateCGroups() {}

  void checkUsage(const SubprocessUsage& usage, bool logging) {
    resourceChecker.checkLimits(usage);
    if (logging) {
      LOG(INFO) << "Current usage"
                << ", numberCpuCores: " << usage.numberCpuCores
                << ", rssMBytes: " << usage.rssMBytes
                << ", numberGpuCores: " << usage.numberGpuCores
                << ", gpuMBytes: " << usage.gpuMBytes;
    }
  }

  void threadFunction(int rounds,
                      SubprocessStats& processor,
                      std::atomic<bool>& starter) {
    while (!starter) {
      std::this_thread::yield();
    }
    while (rounds-- > 0) {
      checkUsage(processor.getUsage(), false);
      std::this_thread::yield();
    }
  }

  void burnCPU(time_t seconds) {
    const auto until = time(nullptr) + seconds;
    double sum = 0.0;
    while (time(nullptr) < until) {
      sum += std::exp(std::log(time(nullptr)));
    }
    CHECK(sum > 0.0);
  }

};

TEST_F(TestSubprocessStats, SingleThread) {
  auto getter = SubprocessStatsCGroupGetter::make(
    CGroupResources(td.path().native(), kSlice, kTask, subSystems));
  EXPECT_NE(getter.get(), nullptr);
  SubprocessStats processor(std::move(getter));
  const int kRounds = 2;
  for (auto idx = 0; idx < kRounds; ++idx ) {
    // consume cpu cycles for 1 second
    burnCPU(1);
    checkUsage(processor.getUsage(), true);
  }
}

TEST_F(TestSubprocessStats, MultiThread) {
  auto getter = SubprocessStatsCGroupGetter::make(
    CGroupResources(td.path().native(), kSlice, kTask, subSystems));
  SubprocessStats processor(std::move(getter), 1);
  const int kRounds = 4*1024;
  std::atomic<bool> started(false);
  // hold threads
  std::thread tOne([&]() {
    threadFunction(kRounds, processor, started);
  });
  std::thread tTwo([&]() {
    threadFunction(kRounds, processor, started);
  });
  started = true;
  tOne.join();
  tTwo.join();
}

TEST_F(TestSubprocessStats, CGroupStats) {
  auto getter = SubprocessStatsCGroupGetter::make(
    CGroupResources(td.path().native(), kSlice, kTask, subSystems));
  EXPECT_NE(getter.get(), nullptr);
  SubprocessStats processor(std::move(getter));
  const int kRounds = 2;
  for (auto idx = 0; idx < kRounds; ++idx ) {
    // consume cpu cycles for 1 second
    burnCPU(1);
    EXPECT_TRUE(folly::writeFile(folly::to<std::string>(
                                 kAppCpuNsec1 * (idx + 1), '\n'),
                                 taskCpuFile.c_str()));
    EXPECT_TRUE(folly::writeFile(folly::to<std::string>(
                                 kAppMemory1 * (idx + 1), '\n'),
                                 taskMemFile.c_str()));


    checkUsage(processor.getUsage(), true);
  }
