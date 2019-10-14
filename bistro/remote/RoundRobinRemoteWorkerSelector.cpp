/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RoundRobinRemoteWorkerSelector.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/monitor/Monitor.h"  // DEFINE_MONITOR_ERROR
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkers.h"

namespace facebook { namespace bistro {

namespace {
bool tryToRun(
    const Job& job,
    const cpp2::BistroWorker& worker,
    int worker_level,
    RemoteWorkerSelector::WorkerResources* worker_resources) noexcept {

  if (!RemoteWorkerSelector::jobCanRunOnWorker(job, worker, worker_level)) {
    return false;
  }

  const auto& job_resources = job.resources();
  const auto& rv_it = worker_resources->find(worker.shard);
  // I'm not sure, but I think this can happen if a new worker was just
  // added, but updateConfig had not yet run for it.
  if (rv_it == worker_resources->end()) {
    LOG(WARNING) << "Not using worker " << worker.shard << " since it does "
      << "not yet have resources";
    return false;
  }

  ResourceVector new_resources(rv_it->second);
  // In Config.cpp, the default worker level resources (and the per-host
  // overrides) are created as vector with indexes from 0 to "maximum
  // resource ID used by the worker level".  Hence, it's okay that we do not
  // look at job_resources[i] for i >= new_resources.size().  Furthermore,
  // many of the indexes in the resource vector belong to non-worker level
  // resources, and are set to INT_MAX.  You can laugh, or cry, but we just
  // pray that jobs use sufficiently small quantities of these unrelated
  // resources, so that we never run out.
  for (int i = 0; i < new_resources.size(); ++i) {
    int& amount = new_resources[i];
    amount -= job_resources[i];
    if (amount < 0) {
      return false;
    }
  }
  rv_it->second.swap(new_resources);
  return true;
}
}  // anonymous namespace

TaskRunnerResponse RoundRobinRemoteWorkerSelector::findWorker(
    const Config*,
    const Job& job,
    const Node& node,
    int worker_level,
    Monitor* monitor,
    WorkerResources* worker_resources,
    RemoteWorkers* workers,
    cpp2::BistroWorker* found_worker,
    int64_t* did_not_run_sequence_num) noexcept {

  DEFINE_MONITOR_ERROR(monitor, error, "RoundRobinRemoteWorkerSelector");
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
