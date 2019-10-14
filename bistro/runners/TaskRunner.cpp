/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/runners/TaskRunner.h"

#include <folly/Random.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/remote/RemoteWorkerState.h"  // workerSuicide helper
#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

using namespace std;

TaskRunner::TaskRunner()
  : schedulerID_(
      apache::thrift::FRAGILE,
      time(nullptr),
      // Seeded from /dev/urandom, which is important given that it would be
      // pretty useless for rand to correlate with startTime.
      folly::Random::rand64(folly::ThreadLocalPRNG())
    ) {
}

TaskRunnerResponse TaskRunner::runTask(
  const Config& config,
  const std::shared_ptr<const Job>& job,
  const Node& node,
  const TaskStatus* prev_status,
  std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  // These pieces of data are always passed to the job, though your runner
  // could add more.
  const auto& path_to_node = node.getPathToNode();
  folly::dynamic job_args = folly::dynamic::object
    ("id", job->name())
    ("path_to_node", folly::dynamic(path_to_node.begin(), path_to_node.end()))
    ("config", job->config())
    ("resources_by_node", folly::dynamic::object())  // populated below
  ;
  if (prev_status) {
    job_args["prev_status"] = prev_status->toDynamic();
  }

  // Capture the essential details for a task in a RunningTask struct.
  cpp2::RunningTask rt;
  rt.job = job->name();
  rt.node = node.name();

  // Record the resources used by this task, see comment on struct RunningTask.
  // Also prepare a dynamic version of the same data to pass to the task.
  const auto& job_resources = job->resources();
  auto& resources_by_node = job_args.at("resources_by_node");
  for (const auto& n : node.traverseUp()) {
    addNodeResourcesToRunningTask(
      &rt,
      &resources_by_node,
      config,
      n.name(),
      n.level(),
      job_resources
    );
  }

  // rt.workerShard is set as needed by runTaskImpl.
  rt.invocationID.startTime = time(nullptr);
  rt.invocationID.rand = folly::Random::rand64(folly::ThreadLocalPRNG());
  if (prev_status) {
    rt.nextBackoffDuration = job->backoffSettings().getNext(
      prev_status->configuredBackoffDuration()  // **not** the effective one
    );
  } else {
    // Make up something. Or, should I leave this unset, and check in Snapshot?
    cpp2::BackoffDuration bd;
    bd.seconds = 0;
    bd.noMoreBackoffs = false;
    rt.nextBackoffDuration = job->backoffSettings().getNext(bd);
  }
  rt.workerSuicideTaskKillWaitMs =
    RemoteWorkerState::workerSuicideTaskKillWaitMs();

  return runTaskImpl(job, node, rt, job_args, cb);
}

cpp2::NodeResources* TaskRunner::addNodeResourcesToRunningTask(
    cpp2::RunningTask* out_rt,
    folly::dynamic* out_resources_by_node,
    const Config& config,
    const std::string& node_name,
    const int node_level,
    const ResourceVector& job_resources) noexcept {

  const auto& resource_ids = config.levelIDToResourceID[node_level];
  // Keep the RunningTask compact by not sending nodes with empty resources
  if (resource_ids.empty()) {
    return nullptr;
  }

  out_rt->nodeResources.emplace_back();
  auto& nr = out_rt->nodeResources.back();
  nr.node = node_name;

  auto& node_resources =
    ((*out_resources_by_node)[nr.node] = folly::dynamic::object());

  for (int resource_id : resource_ids) {
    const auto& rsrc_name = config.resourceNames.lookup(resource_id);
    const auto rsrc_val = job_resources[resource_id];
    nr.resources.emplace_back(apache::thrift::FRAGILE, rsrc_name, rsrc_val);
    node_resources[rsrc_name] = rsrc_val;
  }

  return &out_rt->nodeResources.back();
}

}}
