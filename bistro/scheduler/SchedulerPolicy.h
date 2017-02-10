/*
 *  Copyright (c) 2016-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <memory>
#include <vector>
#include <unordered_map>

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/scheduler/ResourceVector.h"
#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Config;

// Available resource amounts for each node in this NodeGroup, concatenated
// together.  Offsets 0 through (#resources in node group - 1) are the first
// node, etc.  A given node's resources can be found via Node::offset.
using PackedResources = std::vector<int>;
// The NodeGroup ID was formerly known as `level`.
using NodeGroupToPackedResources = std::unordered_map<int, PackedResources>;

class JobWithNodes {
public:
  // Precompute job & node packed resources once, subtract many times.
  struct NodeGroupResources {
    explicit NodeGroupResources(PackedResources* nodes) : nodes_(nodes) {}
    // This job's requirements in this NodeGroup. Has just one entry for
    // each resource.
    PackedResources job_;
    // Stores a pointer into the scheduler's NodeGroupToPackedResources.
    // This saves us a hash-map lookup in the inner loop.
    PackedResources* nodes_;  // Non-const since try_to_schedule modifies it.
  };

  /**
   * WARNING: Stores pointers into the NodeGroupToPackedResources.
   * Therefore, that structure must outlive all JobWithNodes.
   */
  JobWithNodes(const Config&, JobPtr, NodeGroupToPackedResources*);

  JobWithNodes(
    const JobPtr&,
    const std::unordered_map<int, NodeGroupResources>&);

  JobWithNodes(JobWithNodes&&) = default;
  JobWithNodes& operator=(JobWithNodes&&) = default;
  JobWithNodes(const JobWithNodes&) = delete;
  JobWithNodes& operator=(const JobWithNodes&) = delete;

  inline const JobPtr& job() const { return job_; }
  inline const std::unordered_map<int, NodeGroupResources>&
    nodeGroupResources() const { return nodeGroupToResources_; }

  // Exposed because the policies are expected to mutate this list as they go.
  std::vector<const Node*> nodes;

private:
  JobPtr job_;
  std::unordered_map<int, NodeGroupResources> nodeGroupToResources_;
};

struct SchedulerPolicy {
  virtual ~SchedulerPolicy();
  virtual int schedule(std::vector<JobWithNodes>&, TaskRunnerCallback) = 0;
};

}}  // namespace facebook::bistro
