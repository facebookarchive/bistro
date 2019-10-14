/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/scheduler/SchedulerPolicy.h"

#include "bistro/bistro/config/Config.h"

namespace facebook { namespace bistro {

JobWithNodes::JobWithNodes(
    JobPtr job,
    std::unordered_map<int, NodeGroupResources> nodeGroupToResources)
    : job_(std::move(job)),
      nodeGroupToResources_(std::move(nodeGroupToResources)){}

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

}}  // namespace facebook::bistro
