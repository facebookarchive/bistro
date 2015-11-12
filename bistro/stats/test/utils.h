/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "bistro/bistro/stats/SubprocessStats.h"

namespace facebook {
namespace bistro {

class SubprocessStatsChecker {
 public:
   explicit SubprocessStatsChecker(pid_t pid = 0, /* self */
                                   int type = 0 /* default getter */) {
     auto getter = SubprocessStatsGetterFactory::get(pid, type);
     SubprocessSystem available;
     EXPECT_EQ(0, getter->getSystem(&available));

     memoryLimits_.first = 0.0;
     memoryLimits_.second = available.rssMBytes;

     cpuLimits_.first = 0.0;
     cpuLimits_.second = available.numberCpuCores;
   }

   void checkLimits(const SubprocessUsage& usage) const {
     EXPECT_GE(usage.rssMBytes, memoryLimits_.first);
     EXPECT_LE(usage.rssMBytes, memoryLimits_.second);
     EXPECT_GE(usage.numberCpuCores, cpuLimits_.first);
     EXPECT_LE(usage.numberCpuCores, cpuLimits_.second);
   }
 private:
   std::pair<double, double> memoryLimits_;
   std::pair<double, double> cpuLimits_;
};

}} // facebook //bistro
