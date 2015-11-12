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
  // cpu
  uint64_t numberCpuCores{0};
  // memory
  uint64_t rssMBytes{0};
};

struct SubprocessUsage {
  // cpu
  double numberCpuCores{0.};
  // memory
  double rssMBytes{0.};
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
 * class is thread safe, and uses internal caching for the frequent calls
 * once cache gets expired the next caller updates stats and swap caches
 **/
class SubprocessStats : boost::noncopyable {
 public:
  explicit SubprocessStats(std::unique_ptr<SubprocessStatsGetter> getter,
                           uint32_t updateIntervalSec = 1);
  // explicitly update stats
  int refreshStats();
  // if stats are fresh returns cache, otherwise atomically updates new values
  SubprocessUsage getUsage();
  SubprocessSystem getSystem();

  // convert SubprocessUsage to thrift map<PhysicalResources, double>
  static
  std::map<cpp2::PhysicalResources, double> convert(const SubprocessUsage&);
  // convert SubprocessSystem to thrift map<PhysicalResources, int64>
  static
  std::map<cpp2::PhysicalResources, double> convert(const SubprocessSystem&);
 private:
  std::unique_ptr<SubprocessStatsGetter> getter_; // getter
  // two storages, one is active and stores the latest stats data
  // another gets updated by one thread, if stale data detected
  // and then active index gets swapped
  constexpr static size_t kNumStorages = 2;
  SubprocessUsage storage_[kNumStorages];
  const uint32_t updateIntervalSec_;
  std::atomic<uint32_t> lastUpdateTimeSec_;
  std::atomic<int> storageIdx_; // active storage index
  std::atomic<bool> locked_; // lock flag
};

}}
