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
#include "bistro/bistro/stats/SubprocessStats.h"
#include <thread>

using namespace facebook::bistro;

namespace {

void checkUsage(const SubprocessUsage& usage) {
  EXPECT_GE(usage.rssBytes, 0);
  EXPECT_LE(usage.rssBytes, 4*1024*1024*1024ULL);
  EXPECT_GE(usage.totalBytes, 0);
  EXPECT_LE(usage.totalBytes, 256*1024*1024*1024ULL);

  EXPECT_GE(usage.userCpu, 0);
  EXPECT_LE(usage.userCpu, 32);
  EXPECT_GE(usage.sysCpu, 0);
  EXPECT_LE(usage.sysCpu, 32);
  EXPECT_GE(usage.totalCpu, 0);
  EXPECT_LE(usage.totalCpu, 32);
}

void threadFunction(int rounds,
                    SubprocessStats& processor,
                    std::atomic<bool>& starter) {
  while (!starter) {
    std::this_thread::yield();
  }
  while (rounds-- > 0) {
    checkUsage(processor.getStats());
    std::this_thread::yield();
  }
}

}

TEST(TestSubprocessStats, HandleNew) {
  auto getter = SubprocessStatsGetterFactory::get();
  EXPECT_NE(getter.get(), nullptr);
  SubprocessStats processor(std::move(getter));
  checkUsage(processor.getStats());
}

TEST(MutiThreadTestSubprocessStats, HandleNew) {
  auto getter = SubprocessStatsGetterFactory::get();
  SubprocessStats processor(std::move(getter));
  const int kRounds = 4*1024*1024;
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
