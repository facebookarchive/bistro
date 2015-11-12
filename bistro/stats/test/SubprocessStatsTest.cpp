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
#include "bistro/bistro/stats/test/utils.h"
#include <thread>

using namespace facebook::bistro;

namespace {

SubprocessStatsChecker statsChecker;

void checkUsage(const SubprocessUsage& usage, bool logging) {
  statsChecker.checkLimits(usage);
  if (logging) {
    LOG(INFO) << "Current usage"
              << ", numberCpuCores: " << usage.numberCpuCores
              << ", rssMBytes: " << usage.rssMBytes;
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

}

TEST(TestSubprocessStats, HandleNew) {
  auto getter = SubprocessStatsGetterFactory::get();
  EXPECT_NE(getter.get(), nullptr);
  SubprocessStats processor(std::move(getter));
  const int kRounds = 2;
  for (auto idx = 0; idx < kRounds; ++idx ) {
    // consume cpu cycles for 1 second
    const auto until = time(nullptr) + 1;
    double sum = 0.0;
    while (time(nullptr) < until) {
      sum += std::exp(std::log(time(nullptr)));
    }
    CHECK(sum > 0.0);
    checkUsage(processor.getUsage(), true);
  }
}

TEST(MutiThreadTestSubprocessStats, HandleNew) {
  auto getter = SubprocessStatsGetterFactory::get();
  SubprocessStats processor(std::move(getter));
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
