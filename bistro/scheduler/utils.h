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

#include "bistro/bistro/scheduler/SchedulerPolicy.h"
#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Node;
class Job;

typedef std::shared_ptr<const Job> JobPtr;
typedef std::shared_ptr<const Node> NodePtr;

TaskRunnerResponse try_to_schedule(
  ResourcesByNodeType& resources_by_node,
  const NodePtr& node_ptr,
  const JobPtr& job,
  TaskRunnerCallback cb
);

}}
