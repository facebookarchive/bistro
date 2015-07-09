/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/config/SchedulerType.h"

#include <unordered_map>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

namespace {
  std::unordered_map<folly::fbstring, SchedulerType> type_map = {
    { "roundrobin", SchedulerType::RoundRobin },
    { "ranked_priority", SchedulerType::RankedPriority },
    { "randomized_priority", SchedulerType::RandomizedPriority },
    { "long_tail", SchedulerType::LongTail },
  };
}

SchedulerType getSchedulerType(const folly::fbstring& s) {
  auto it = type_map.find(s);
  if (it == type_map.end()) {
    throw BistroException("Unknown scheduler type: ", s);
  }
  return it->second;
}

}}
