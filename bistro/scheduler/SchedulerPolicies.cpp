/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    std::make_unique<UnitTestSchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyRoundRobin.str(),
    std::make_unique<RoundRobinSchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyRankedPriority.str(),
    std::make_unique<RankedPrioritySchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyRandomPriority.str(),
    std::make_unique<RandomizedPrioritySchedulerPolicy>()
  );
  registerSchedulerPolicy(
    kSchedulePolicyLongTail.str(),
    std::make_unique<LongTailSchedulerPolicy>()
  );
}

}}  // namespace facebook::bistro
