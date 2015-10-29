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
SubprocessStatsGetterFactory::get(pid_t pid, int type, bool init) {
  std::unique_ptr<SubprocessStatsGetter> getter =
      folly::make_unique<SubprocessStatsSigarGetter>(pid);
  if (init && getter->initialize() != SIGAR_OK) {
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
  memset(storage_, 0, sizeof(storage_));
}

int SubprocessStats::refreshStats() {
  // try to lock
  bool locked = false;
  if (!locked_.compare_exchange_strong(locked, true)) {
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
  locked_.compare_exchange_strong(locked, false);
  return res;
}

SubprocessUsage SubprocessStats::getStats() {
  const auto now = time(nullptr);
  const auto lastUpdate = lastUpdateTimeSec_.load();
  // check cache expiration
  if (now >= lastUpdate + updateIntervalSec_) { // stale cache
    refreshStats();
  }
  // return active storage
  return storage_[storageIdx_.load()];
}

}}
