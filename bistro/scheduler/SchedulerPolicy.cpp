/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/SchedulerPolicy.h"

#include "bistro/bistro/config/SchedulerType.h"
#include "bistro/bistro/scheduler/LongTailSchedulerPolicy.h"
#include "bistro/bistro/scheduler/RandomizedPrioritySchedulerPolicy.h"
#include "bistro/bistro/scheduler/RankedPrioritySchedulerPolicy.h"
#include "bistro/bistro/scheduler/RoundRobinSchedulerPolicy.h"
#include "bistro/bistro/utils/EnumHash.h"

namespace facebook { namespace bistro {

using namespace std;

namespace {
  typedef shared_ptr<SchedulerPolicy> Ptr;
  unordered_map<SchedulerType, Ptr, EnumHash> policies = {
    { SchedulerType::RoundRobin, Ptr(new RoundRobinSchedulerPolicy()) },
    { SchedulerType::RankedPriority, Ptr(new RankedPrioritySchedulerPolicy()) },
    { SchedulerType::RandomizedPriority, Ptr(new RandomizedPrioritySchedulerPolicy()) },
    { SchedulerType::LongTail, Ptr(new LongTailSchedulerPolicy()) },
  };
}

SchedulerPolicy::~SchedulerPolicy() {
}

SchedulerPolicy* SchedulerPolicy::getSingleton(SchedulerType type) {
  return policies.find(type)->second.get();
}

}}
