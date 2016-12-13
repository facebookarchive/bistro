/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/SchedulerPolicy.h"

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/scheduler/LongTailSchedulerPolicy.h"
#include "bistro/bistro/scheduler/RandomizedPrioritySchedulerPolicy.h"
#include "bistro/bistro/scheduler/RankedPrioritySchedulerPolicy.h"
#include "bistro/bistro/scheduler/RoundRobinSchedulerPolicy.h"
#include "bistro/bistro/scheduler/UnitTestSchedulerPolicy.h"  // A mock

namespace facebook { namespace bistro {

namespace {
  std::unordered_map<std::string, std::shared_ptr<SchedulerPolicy>> policies = {
    { kSchedulePolicyUnitTest.str(),
        std::make_shared<UnitTestSchedulerPolicy>() },
    { kSchedulePolicyRoundRobin.str(),
        std::make_shared<RoundRobinSchedulerPolicy>() },
    { kSchedulePolicyRankedPriority.str(),
        std::make_shared<RankedPrioritySchedulerPolicy>() },
    { kSchedulePolicyRandomPriority.str(),
        std::make_shared<RandomizedPrioritySchedulerPolicy>() },
    { kSchedulePolicyLongTail.str(),
        std::make_shared<LongTailSchedulerPolicy>() },
  };
}

JobWithNodes::JobWithNodes(
    const Config& cfg,
    JobPtr j,
    NodeGroupToPackedResources* packed_resources) : job_(std::move(j)) {
  for (size_t ngid = 0; ngid < packed_resources->size(); ++ngid) {
    auto p = nodeGroupToResources_.emplace(std::make_pair(
      ngid, NodeGroupResources(&(*packed_resources)[ngid])
    ));
    CHECK(p.second);
    // The resource indices must match those in Scheduler::schedule's
    // NodeGroupToPackedResources and NodeGroup::resourceToIndex_.
    const auto& rids = cfg.levelIDToResourceID[ngid];
    for (size_t i = 0; i < rids.size(); ++i) {
      p.first->second.job_.emplace_back(job_->resources()[rids[i]]);
    }
  }
}

SchedulerPolicy::~SchedulerPolicy() {
}

void SchedulerPolicy::registerSchedulerPolicy(
    std::string scheduler_policy_name,
    std::unique_ptr<SchedulerPolicy> policy) {
  auto p = policies.emplace(
    std::move(scheduler_policy_name), std::move(policy));
  CHECK(p.second) << "scheduler_policy_name "
                  << p.first->first << " is already registered";
}

SchedulerPolicy* SchedulerPolicy::getSingleton(
    const std::string& scheduler_policy_name) {
  auto it = policies.find(scheduler_policy_name);

  if (it == policies.end()) {
    throw BistroException(
      "scheduler policy", scheduler_policy_name, " is not registered");
  }

  return it->second.get();
}

}}
