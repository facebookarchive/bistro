/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/utils.h"

#include <string>

#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/Job.h"

namespace facebook { namespace bistro {

using namespace std;

/**
 * Try to schedule the given node and job, satisfying resource constraints. If
 * it's possible to schedule it, call the callback with that task, and if it was
 * actually run, modify the passed in resource array.
 */
TaskRunnerResponse try_to_schedule(
    ResourcesByNodeType& resources_by_node,
    const NodePtr& node_ptr,
    const JobPtr& job_ptr,
    TaskRunnerCallback cb) {

  // Store the potential new resources for each node. We'll only actually modify
  // them at the end if we are able to satisfy all the resource constraints.
  ResourcesByNodeType new_resources_by_node;

  auto& job_resources = job_ptr->resources();
  for (const auto& node : node_ptr->traverseUp()) {
    new_resources_by_node[node.id()] = resources_by_node[node.id()];
    auto& cur_resources = new_resources_by_node[node.id()];
    for (int i = 0; i < cur_resources.size() && i < job_resources.size(); ++i) {
      int& amount = cur_resources[i];
      amount -= job_resources[i];
      if (amount < 0) {
        return TaskRunnerResponse::DidNotRunTask;
      }
    }
  }

  const auto ret = cb(job_ptr, node_ptr);
  if (ret == TaskRunnerResponse::RanTask) {
    for (const auto& pair : new_resources_by_node) {
      resources_by_node[pair.first] = pair.second;
    }
  }
  return ret;
}

}}
