/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/scheduler/utils.h"

#include <string>

#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/scheduler/SchedulerPolicy.h"

namespace facebook { namespace bistro {

using namespace std;

/**
 * Try to schedule the given node and job, satisfying resource constraints. If
 * it's possible to schedule it, call the callback with that task, and if it was
 * actually run, modify the passed in resource array.
 */
TaskRunnerResponse try_to_schedule(
    const Node& task_node,
    const JobWithNodes& jwn,
    TaskRunnerCallback cb) {

  std::vector<std::pair<int*, int>> updates;

  for (const auto& node : task_node.traverseUp()) {
    auto& resources = jwn.nodeGroupResources().at(node.level());
    CHECK_GE(resources.nodes_->size(), node.offset + resources.job_.size());
    for (size_t i = 0; i < resources.job_.size(); ++i) {
      auto& node_resource = (*resources.nodes_)[node.offset + i];
      if (resources.job_[i] > node_resource) {
        return TaskRunnerResponse::DidNotRunTask;
      }
      updates.emplace_back(std::make_pair(&node_resource, resources.job_[i]));
    }
  }

  const auto ret = cb(jwn.job(), task_node);
  if (ret == TaskRunnerResponse::RanTask) {
    for (auto& p : updates) {
      CHECK_GE(*p.first, p.second);
      *p.first -= p.second;
    }
  }
  return ret;
}

}}
