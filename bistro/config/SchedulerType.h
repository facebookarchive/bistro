#pragma once

#include "folly/FBString.h"

namespace facebook { namespace bistro {

enum class SchedulerType {
  RoundRobin,
  RankedPriority,
  RandomizedPriority,
  LongTail
};

SchedulerType getSchedulerType(const folly::fbstring& s);

}}
