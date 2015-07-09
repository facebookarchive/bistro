/*
 *  Copyright (c) 2015, Facebook, Inc.
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

#include "bistro/bistro/config/SchedulerType.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/scheduler/ResourceVector.h"
#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

struct JobWithNodes {
  JobPtr job;
  std::vector<NodePtr> nodes;

  JobWithNodes() {}
  JobWithNodes(JobPtr j, std::vector<NodePtr> n)
    : job(std::move(j)), nodes(std::move(n)) {}

  JobWithNodes(JobWithNodes&&) = default;
  JobWithNodes& operator=(JobWithNodes&&) = default;
  JobWithNodes(const JobWithNodes&) = delete;
  JobWithNodes& operator=(const JobWithNodes&) = delete;
};

typedef std::unordered_map<Node::ID, ResourceVector> ResourcesByNodeType;

class SchedulerPolicy {

public:
  virtual int schedule(
    std::vector<JobWithNodes>& jobs,
    ResourcesByNodeType& resources_by_node,
    TaskRunnerCallback cb
  ) = 0;

  virtual ~SchedulerPolicy();

  static SchedulerPolicy* getSingleton(SchedulerType type);

};


}}
