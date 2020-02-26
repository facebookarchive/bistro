/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/Bistro.h"

#include <folly/experimental/AutoTimer.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/config/ConfigLoader.h"
#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/runners/TaskRunner.h"
#include "bistro/bistro/scheduler/Scheduler.h"

DECLARE_bool(log_performance);

namespace facebook { namespace bistro {

using namespace std;

namespace {
  const chrono::seconds kSleepOnError(5);
}

Bistro::Bistro(
    shared_ptr<ConfigLoader> config_loader,
    shared_ptr<NodesLoader> nodes_loader,
    shared_ptr<TaskStatuses> task_statuses,
    shared_ptr<TaskRunner> task_runner,
    shared_ptr<Monitor> monitor)
  : configLoader_(config_loader),
    nodesLoader_(nodes_loader),
    taskStatuses_(task_statuses),
    taskRunner_(task_runner),
    monitor_(monitor) {}

std::chrono::milliseconds Bistro::scheduleOnce(
    std::chrono::milliseconds time_since_epoch) noexcept {
  DEFINE_MONITOR_ERROR(monitor_.get(), error, "Main loop");

  std::shared_ptr<const Config> config;
  try {
    config = configLoader_->getDataOrThrow();
    if (!config->enabled) {
      LOG(WARNING) << error.report("Not running because enabled is false");
      return config->idleWait;
    }
  } catch (const exception& e) {
    LOG(ERROR) << error.report("Error updating config: ", e.what());
    return kSleepOnError;
  }

  // DO: Get the Nodes before getting the Config so that the our config is
  // always as new as the one used to fetch the nodes, or newer.  It's not
  // desirable to hold back the config to be exactly the same version as the
  // one used to fetch the nodes, since nodes are polled much less often,
  // and it's best to be maximally responsive to config changes.
  std::shared_ptr<const Nodes> nodes;
  try {
    nodes = nodesLoader_->getDataOrThrow();
    if (nodes->size() == 0) {
      LOG(WARNING) << error.report("Not running because no nodes are defined");
      return config->idleWait;
    }
  } catch (const exception& e) {
    LOG(ERROR) << error.report("Error getting nodes: ", e.what());
    return kSleepOnError;
  }

  // Load statuses from TaskStore, free status memory used by deleted jobs.
  taskStatuses_->updateForConfig(*config);
  // Updates remote worker resources. It would be great to make remote
  // worker resources be modeled as regular resources in the scheduler, so
  // there is no need to update worker resources separately.
  taskRunner_->updateConfig(config);

  auto status_snapshot = taskStatuses_->copySnapshot();
  auto sched_result = Scheduler().schedule(
    std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count(),
    *config,
    nodes,
    status_snapshot,
    [this, &status_snapshot, config]
    (const JobPtr& job, const Node& node) noexcept {
      // The lifetime of the inner callback is potentially very long, so
      // just copy and capture the data that it needs.
      const auto job_id = job->id();
      const auto node_id = node.id();
      return taskRunner_->runTask(
        *config,
        job,
        node,
        // The previous status, if any.
        status_snapshot.getPtr(job->id(), node.id()),
        [this, job_id, node_id](
          const cpp2::RunningTask& running_task, TaskStatus&& status
        ) noexcept {
          // TODO(#5507329): The reason this exists (as opposed to passing
          // around taskStatuses_ is that for "in-process" updateStatus
          // calls, we always have the numeric node & job IDs available, so
          // this is an unbenchmarked micro-optimization to reuse them
          // instead of looking them up.  The code would be simpler without.
          // For example, RemoteWorker could directly accept a TaskStatuses,
          // thus ensuring atomicity of updates, and avoiding the
          // complicated "RemoteWorkerUpdate" scheme.
          taskStatuses_->updateStatus(
            job_id,
            node_id,
            running_task,
            std::move(status)
          );
        }
      );
    },
    monitor_
  );

  folly::AutoTimer<> timer;
  // We'll make a new map, discarding any tasks that are no longer orphans.
  decltype(orphanTaskIDToKillTime_)  new_id_to_kill_time;
  size_t num_killed = 0;
  if (taskRunner_->canKill()) {
    // Is it time to kill any of the currently orphaned tasks?
    for (auto&& rt : sched_result.orphanTasks_) {
      std::chrono::milliseconds kill_after;
      auto jit = config->jobs.find(rt.job);
      if (jit == config->jobs.end()) {
        // Job deleted? No problem, default to the deployment-wide policy.
        if (!config->killOrphanTasksAfter.has_value()) {
          continue;
        }
        kill_after = config->killOrphanTasksAfter.value();
      } else {
        if (!jit->second->killOrphanTasksAfter().has_value()) {
          continue;
        }
        kill_after = jit->second->killOrphanTasksAfter().value();
      }
      auto id = std::make_pair(rt.job, rt.node);
      auto it = orphanTaskIDToKillTime_.find(id);
      auto kill_time = (it == orphanTaskIDToKillTime_.end())
        ? (time_since_epoch + kill_after) : it->second;
      if (kill_time > time_since_epoch) {  // Kill later
        new_id_to_kill_time[std::move(id)] = kill_time;
      } else {  // Kill now
        // Future: Consider setting a delayed "kill time" for the
        // just-killed task to ensure that we don't try to kill stubborn
        // tasks in *every* scheduling loop.
        try {
          taskRunner_->killTask(
            rt,
            jit != config->jobs.end()  // Fall back to the global kill request
              ? jit->second->killRequest() : config->killRequest
          );
        } catch (const std::exception& ex) {
          LOG(WARNING) << "Failed to kill orphan task "
            << apache::thrift::debugString(rt) << ": " << ex.what();
        }
        ++num_killed;
      }
    }
  }  // Else: forget all orphans, since the feature was turned off
  orphanTaskIDToKillTime_ = new_id_to_kill_time;
  if (FLAGS_log_performance && !sched_result.orphanTasks_.empty()) {
    timer.log(
      "Processed ", sched_result.orphanTasks_.size(), " orphan tasks and "
      "tried to kill ", num_killed
    );
  }

  if (sched_result.areTasksRunning_) {
    LOG(INFO) << "Working wait...";
    return config->workingWait;
  }
  LOG(INFO) << "Idle wait...";
  return config->idleWait;
}

}}
