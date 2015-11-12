/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/stats/SubprocessStats.h"
#include "bistro/bistro/stats/SubprocessStatsSigarGetter.h"
#include <folly/Memory.h>
#include <glog/logging.h>

namespace facebook { namespace bistro {

///////////////////// - SubprocessStatsGetterFactory
std::unique_ptr<SubprocessStatsGetter>
SubprocessStatsGetterFactory::get(pid_t pid, int, bool init) {
  SubprocessStatsSigarGetter* obj = new SubprocessStatsSigarGetter(pid);
  std::unique_ptr<SubprocessStatsGetter> getter(obj);
  if (init && !obj->initialize()) {
    LOG(ERROR) << "Cannot initialize SubprocessStatsSigarGetter, pid: " << pid;
    getter.reset();
  }
  return getter;
}


///////////////////// - SubprocessStats
SubprocessStats::SubprocessStats(std::unique_ptr<SubprocessStatsGetter> getter,
                                 uint32_t updateIntervalSec)
 : getter_(std::move(getter))
 , updateIntervalSec_(updateIntervalSec)
 , lastUpdateTimeSec_(0)
 , storageIdx_(0)
 , locked_(false) {
}

int SubprocessStats::refreshStats() {
  // try to lock
  bool locked = false;
  if (!(locked = locked_.compare_exchange_strong(locked, true))) {
    LOG(WARNING) << "Cannot get the lock, another thread owns it";
    return 1; // failed to get the lock
  }

  // update last time right away
  lastUpdateTimeSec_.store(time(nullptr));
  // get active storage index
  const auto idxActive = storageIdx_.load();
  // calculate passive storage index
  const auto idxPassive = (idxActive + 1) % kNumStorages;
  // under lock it is safe to call the getter
  // and fill out passive storage
  const auto res = getter_->getUsage(&storage_[idxPassive]);
  if (res == 0) {
    // success, flip storage index
    storageIdx_.store(idxPassive);
  } else {
    LOG(ERROR) << "Cannot get resources, error code: " << res;
  }
  // release the lock
  CHECK(locked_.compare_exchange_strong(locked, false));
  return res;
}

SubprocessUsage SubprocessStats::getUsage() {
  const auto now = time(nullptr);
  const auto lastUpdate = lastUpdateTimeSec_.load();
  // check cache expiration
  if (now >= lastUpdate + updateIntervalSec_) { // stale cache
    refreshStats();
  }
  // return active storage
  return storage_[storageIdx_.load()];
}

SubprocessSystem SubprocessStats::getSystem() {
  // getter would cache system resources
  SubprocessSystem resources;
  getter_->getSystem(&resources);
  return resources;
}

/* static */ std::map<cpp2::PhysicalResources, double>
SubprocessStats::convert(const SubprocessUsage& usage) {
  std::map<cpp2::PhysicalResources, double> res;
  res[cpp2::PhysicalResources::RAM_MBYTES] = usage.rssMBytes;
  res[cpp2::PhysicalResources::CPU_CORES] = usage.numberCpuCores;
  return res;
}

/* static */ std::map<cpp2::PhysicalResources, double>
SubprocessStats::convert(const SubprocessSystem& resources) {
  std::map<cpp2::PhysicalResources, double> res;
  res[cpp2::PhysicalResources::RAM_MBYTES] = resources.rssMBytes;
  res[cpp2::PhysicalResources::CPU_CORES] = resources.numberCpuCores;
  return res;
}

}}
