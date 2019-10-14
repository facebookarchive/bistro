/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BusiestRemoteWorkerSelector.h"

#include <limits>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/monitor/Monitor.h"  // DEFINE_MONITOR_ERROR
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkers.h"

namespace facebook { namespace bistro {

TaskRunnerResponse BusiestRemoteWorkerSelector::findWorker(
    const Config* config,
    const Job& job,
    const Node& node,
    int worker_level,
    Monitor* monitor,
    WorkerResources* worker_resources,
    RemoteWorkers* workers,
    cpp2::BistroWorker* found_worker,
    int64_t* did_not_run_sequence_num) noexcept {

  DEFINE_MONITOR_ERROR(monitor, error, "BusiestRemoteWorkerSelector");
  // Optionally, respect task-locality requirements
  auto hostname = job.requiredHostForTask(node);

  const auto& worker_pool = hostname.empty()
    ? workers->workerPool() : workers->hostWorkerPool(hostname);
  const auto& worker_resource_ids =
    config->levelIDToResourceID.at(worker_level);

  // `int` since ResourceVector stores int, but `int64_t` could make sense too.
  using Weight = int;
  Weight least_weight = std::numeric_limits<Weight>::max();
  RemoteWorker* busiest_worker = nullptr;  // This worker can fit this task.
  ResourceVector* busiest_worker_rsrc = nullptr;

  // If no worker is healthy, don't bother trying to run more tasks.
  bool saw_healthy_worker = false;

  // Compute weights for workers so that the busiest one has the lowest weight.
  for (const auto& p : worker_pool) {
    if (!p.second->isHealthy()) {
      continue;
    }
    auto rv_it = worker_resources->find(p.first);
    // I'm not sure, but I think this can happen if a new worker was just
    // added, but updateConfig had not yet run for it.
    if (rv_it == worker_resources->end()) {
      LOG(WARNING) << "Not using worker " << p.first << " since it does "
        << "not yet have resources";
      continue;
    }
    saw_healthy_worker = true;

    if (!RemoteWorkerSelector::jobCanRunOnWorker(
      job, p.second->getBistroWorker(), worker_level
    )) {
      continue;
    }

    // If the task fits on the worker, compute the left-over weight.
    Weight remaining_weight = 0;
    for (auto rid : worker_resource_ids) {
      CHECK(rid >= 0 && rid < rv_it->second.size()) << rid;
      CHECK(rid < config->resourceIDToWeight.size()) << rid;
      CHECK(rv_it->second[rid] != std::numeric_limits<int>::max()) << rid;
      CHECK(rid < job.resources().size());
      if (rv_it->second[rid] < job.resources()[rid]) {
        remaining_weight = -1;  // Sentinel: Not enough resources to run task.
        break;
      }
      // Can never become negative since weights are nonnegative.
      remaining_weight += (rv_it->second[rid] - job.resources()[rid])
        * config->resourceIDToWeight[rid];
    }

    if (remaining_weight >= 0 && remaining_weight < least_weight) {
      least_weight = remaining_weight;
      busiest_worker = p.second.get();
      busiest_worker_rsrc = &rv_it->second;
    }
  }

  if (!busiest_worker) {
    // We weren't able to schedule this task. But future tasks could still
    // run if they use fewer resources.
    if (saw_healthy_worker) {
      return DidNotRunTask;
    }
    LOG(WARNING) << error.report(
      "No healthy workers to run ", job.name(), ", ", node.name()
    );
    return DoNotRunMoreTasks;
  }
  CHECK(busiest_worker_rsrc);

  *found_worker = busiest_worker->getBistroWorker();
  *did_not_run_sequence_num =
    busiest_worker->getNotifyIfTasksNotRunningSequenceNum();
  // Update the worker's resource vector.
  for (auto rid : worker_resource_ids) {
    CHECK_GE((*busiest_worker_rsrc)[rid], job.resources()[rid]);
    (*busiest_worker_rsrc)[rid] -= job.resources()[rid];
  }
  return RanTask;
}

}}  // namespace facebook::bistro
