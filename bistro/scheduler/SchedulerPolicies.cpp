/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/SchedulerPolicies.h"

#include "bistro/bistro/config/Config.h"  // for the constants, lol
#include "bistro/bistro/scheduler/LongTailSchedulerPolicy.h"
#include "bistro/bistro/scheduler/RandomizedPrioritySchedulerPolicy.h"
#include "bistro/bistro/scheduler/RankedPrioritySchedulerPolicy.h"
#include "bistro/bistro/scheduler/RoundRobinSchedulerPolicy.h"
#include "bistro/bistro/scheduler/SchedulerPolicyRegistry.h"
#include "bistro/bistro/scheduler/UnitTestSchedulerPolicy.h"  // A mock

namespace facebook { namespace bistro {

void registerDefaultSchedulerPolicies() {
  registerSchedulerPolicy(
    kSchedulePolicyUnitTest.str(),
    folly::make_unique<UnitTestSchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyRoundRobin.str(),
    folly::make_unique<RoundRobinSchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyRankedPriority.str(),
    folly::make_unique<RankedPrioritySchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyRandomPriority.str(),
    folly::make_unique<RandomizedPrioritySchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyLongTail.str(),
    folly::make_unique<LongTailSchedulerPolicy>()
  );
}

}}  // namespace facebook::bistro
