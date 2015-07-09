/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "RemoteWorkerSelector.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/monitor/Monitor.h"  // DEFINE_MONITOR_ERROR
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkers.h"
#include "bistro/bistro/remote/RoundRobinRemoteWorkerSelector.h"
#include "bistro/bistro/remote/BusiestRemoteWorkerSelector.h"
#include "bistro/bistro/utils/EnumHash.h"

namespace facebook { namespace bistro {

namespace {
  typedef std::shared_ptr<RemoteWorkerSelector> Ptr;
  std::unordered_map<RemoteWorkerSelectorType, Ptr, EnumHash> policies = {
    {
      RemoteWorkerSelectorType::RoundRobin,
      Ptr(new RoundRobinRemoteWorkerSelector())
    },
    {
      RemoteWorkerSelectorType::Busiest,
      Ptr(new BusiestRemoteWorkerSelector())
    }
  };
}

RemoteWorkerSelector::~RemoteWorkerSelector() {}

RemoteWorkerSelector* RemoteWorkerSelector::getSingleton(
    RemoteWorkerSelectorType type) {

  return policies.find(type)->second.get();
}

bool RemoteWorkerSelector::tryToRun(
    const Job& job,
    const cpp2::BistroWorker& worker,
    int worker_level,
    WorkerResources* worker_resources) noexcept {

  CHECK(worker_level < job.filters().size());
  if (!job.filters()[worker_level].doesPass(
    job.name(), worker.machineLock.hostname
  )) {
    return false;
  }
  const auto& job_resources = job.resources();
  auto& resource_vector = (*worker_resources)[worker.shard];

  ResourceVector new_resources(resource_vector);
  for (int i = 0; i < new_resources.size(); ++i) {
    int& amount = new_resources[i];
    amount -= job_resources[i];
    if (amount < 0) {
      return false;
    }
  }
  resource_vector.swap(new_resources);
  return true;
}

TaskRunnerResponse RemoteWorkerSelector::findWorker(
    const Job& job,
    const Node& node,
    int worker_level,
    Monitor* monitor,
    WorkerResources* worker_resources,
    RemoteWorkers* workers,  // XXX why is this mutable?
    cpp2::BistroWorker* found_worker,
    // its value on the found worker -- XXX better doc, better name
    int64_t* did_not_run_sequence_num) noexcept {

  DEFINE_MONITOR_ERROR(monitor, error, "RemoteWorkerRunner worker search");
  // Optionally, respect task-locality requirements
  auto hostname = job.requiredHostForTask(node);

  const auto* first_worker = hostname.empty()
    ? workers->getNextWorker()
    : workers->getNextWorkerByHost(hostname);
  if (!first_worker) {
    LOG(WARNING) << error.report(
      "No workers were available to run ", job.name(), ", ", node.name()
    );
    return DoNotRunMoreTasks;
  }

  // At least one worker is available, so search through them until we
  // come back to the beginning.
  const auto* worker = first_worker;
  bool saw_healthy_worker = false;
  do {
    if (worker->isHealthy()) {
      saw_healthy_worker = true;
      const auto& bistro_worker = worker->getBistroWorker();
      if (tryToRun(job, bistro_worker, worker_level, worker_resources)) {
        *found_worker = bistro_worker;
        *did_not_run_sequence_num =
          worker->getNotifyIfTasksNotRunningSequenceNum();
        return RanTask;
      }
    }
    // This worker didn't have enough resources, on to the next one
    worker = hostname.empty()
      ? workers->getNextWorker()
      : workers->getNextWorkerByHost(hostname);
    CHECK(worker) << "Workers cannot disappear during search";
  } while (worker != first_worker);

  if (!saw_healthy_worker) {
    LOG(WARNING) << error.report(
      "None of the workers are healthy to run ", job.name(), ", ", node.name()
    );
    return DoNotRunMoreTasks;
  }

  // We weren't able to schedule this task. But future tasks could still run
  // if they use fewer resources.
  return DidNotRunTask;
}

}}  // namespace facebook::bistro
