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

#include <boost/noncopyable.hpp>
#include <atomic>
#include <memory>
#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

struct SubprocessSystem {
  // cpu cores
  uint64_t numberCpuCores{0};
  // RAM (MB)
  uint64_t rssMBytes{0};
  // gpu cores
  uint64_t numberGpuCores{0};
  // gpu memory (MB)
  uint64_t gpuMBytes{0};
};

struct SubprocessUsage {
  // cpu cores
  double numberCpuCores{0.};
  // RAM (MB)
  double rssMBytes{0.};
  // gpu cores
  double numberGpuCores{0.};
  // gpu memory (MB)
  double gpuMBytes{0.};
};

/**
 * Interface to extract subprocess resources stats,
 * i.e. RAM, CPU, Network usage.
 * Implementation can ignore thread safety
 */
class SubprocessStatsGetter {
 public:
   virtual ~SubprocessStatsGetter() {}
   // first call, should be done before getUsage
   // returns linux system err or 0 on success
   virtual int getSystem(SubprocessSystem* available) = 0;
   // refresh system stats, usually system stats are constant
   // however some stats like GPUs can be lost due to nvidia software bugs.
   virtual void checkSystem() = 0;
   // returns linux system err or 0 on success
   virtual int getUsage(SubprocessUsage* usage) = 0;
};

class SubprocessStatsGetterFactory {
 public:
  static std::unique_ptr<SubprocessStatsGetter> get(
    pid_t pid = 0, // self
    int type = 0, // default - reserved,
    bool init = true // initialize on start or not
  );
};

/**
 * This class is provides synchronization and caching for a per-PID getter
 * (which need not be thread-safe). The getter does the actual work of getting
 * system and per-PID resources.
 **/
class SubprocessStats : boost::noncopyable {
 public:
  explicit SubprocessStats(std::unique_ptr<SubprocessStatsGetter> getter,
                           time_t processUpdateIntervalSec = 1,
                           time_t systemUpdateIntervalSec = 900);
  // if stats are fresh returns cache, otherwise atomically updates new values
  SubprocessUsage getUsage();
  SubprocessSystem getSystem();

  // convert SubprocessUsage to thrift map<PhysicalResource, double>
  static
  std::map<cpp2::PhysicalResource, double> convert(const SubprocessUsage&);
  // convert SubprocessSystem to thrift map<PhysicalResource, int64>
  static
  std::map<cpp2::PhysicalResource, double> convert(const SubprocessSystem&);
 private:
   // explicitly update stats
   int refreshStats(time_t now);

 private:
  std::unique_ptr<SubprocessStatsGetter> getter_; // getter
  // two storages, one is active and stores the latest stats data
  // another gets updated by one thread, if stale data detected
  // and then active index gets swapped
  constexpr static size_t kNumStorages = 2;
  SubprocessUsage storage_[kNumStorages];
  const time_t processUpdateIntervalSec_;
  const time_t systemUpdateIntervalSec_;
  std::atomic<time_t> lastProcessUpdateTimeSec_{0};
  std::atomic<time_t> lastSystemUpdateTimeSec_{0};
  std::atomic<int> storageIdx_{0}; // active storage index
  std::atomic<bool> locked_{false}; // lock flag
};

}}
