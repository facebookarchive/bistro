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

     rssMemoryLimits_.first = 0.0;
     rssMemoryLimits_.second = available.rssMBytes;
     cpuCoresLimits_.first = 0.0;
     cpuCoresLimits_.second = available.numberCpuCores;
     gpuMemoryLimits_.first = 0.0;
     gpuMemoryLimits_.second = available.gpuMBytes;
     gpuCoresLimits_.first = 0.0;
     gpuCoresLimits_.second = available.numberGpuCores;
   }

   void checkLimits(const SubprocessUsage& usage) const {
     EXPECT_GE(usage.rssMBytes, rssMemoryLimits_.first);
     EXPECT_LE(usage.rssMBytes, rssMemoryLimits_.second);
     EXPECT_GE(usage.numberCpuCores, cpuCoresLimits_.first);
     EXPECT_LE(usage.numberCpuCores, cpuCoresLimits_.second);
     EXPECT_GE(usage.gpuMBytes, gpuMemoryLimits_.first);
     EXPECT_LE(usage.gpuMBytes, gpuMemoryLimits_.second);
     EXPECT_GE(usage.numberGpuCores, gpuCoresLimits_.first);
     EXPECT_LE(usage.numberGpuCores, gpuCoresLimits_.second);
   }
 private:
   std::pair<double, double> rssMemoryLimits_;
   std::pair<double, double> cpuCoresLimits_;
   std::pair<double, double> gpuMemoryLimits_;
   std::pair<double, double> gpuCoresLimits_;
};

}} // facebook //bistro
