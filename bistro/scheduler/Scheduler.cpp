/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/Scheduler.h"

#include <folly/experimental/AutoTimer.h>
#include <list>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/SchedulerType.h"
#include "bistro/bistro/flags/Flags.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/scheduler/SchedulerPolicy.h"
#include "bistro/bistro/statuses/TaskStatusSnapshot.h"

namespace facebook { namespace bistro {

using apache::thrift::debugString;
using namespace std;

Scheduler::Result Scheduler::schedule(
    time_t cur_time,
    const Config& config,
    const std::shared_ptr<const Nodes>& nodes,
    const TaskStatusSnapshot& status_snapshot,
    TaskRunnerCallback cb,
    const std::shared_ptr<Monitor> monitor) {

  DEFINE_MONITOR_ERROR(monitor, error, "Running task resources");
  folly::AutoTimer<> timer;
  Scheduler::Result result;

  // Compute total resources for each node.
  ResourcesByNodeType resources_by_node;
  for (const auto& node : *nodes) {
    resources_by_node[node->id()] = config.resourcesByLevel[node->level()];
  }
  if (FLAGS_log_performance) {
    timer.log("Resources computed");
  }

  const auto& running_tasks(status_snapshot.getRunningTasks());
  // For node "orphans": running tasks, whose nodes are deleted or disabled.
  // Using 'list' for stable iterators.
  std::unordered_map<Node::ID, std::list<cpp2::RunningTask>> node_id_to_tasks;
  for (const auto& id_and_task : running_tasks) {
    const auto& rt = id_and_task.second;
    node_id_to_tasks[id_and_task.first.second].push_front(rt);
    // Subtract out resources used by the running tasks. We do this in a
    // outside of the cross-product loop, because:
    //  - this way, we account even for tasks from deleted jobs
    //  - running_tasks.find() for every job-node combination is expensive
    //
    // DO(#4936759): Since workers only send their running task with the
    // initial connection, it would be best to perform error-checking and a
    // conversion to a faster representation of running task resources at
    // that point, and only use that here.  Also, should we, as a matter of
    // policy, kill tasks that have bad resource metadata?
    for (const auto& nr : rt.nodeResources) {
      // Until workers & nodes are fully merged, worker resource tracking
      // happens in RemoteWorkerRunner.
      if (nr.node == rt.workerShard) {
        continue;
      }
      int node_id = Node::NodeNameTable.asConst()->lookup(nr.node);
      if (node_id == StringTable::NotFound) {
        LOG(ERROR) << error.report(
          "Unknown node ", nr.node, " in ", debugString(rt)
        );
        continue;
      }
      auto it = resources_by_node.find(Node::ID(node_id));
      if (it == resources_by_node.end()) {
        LOG(ERROR) << error.report(
          "Node without resources: ", nr.node, " from ", debugString(rt)
        );
        continue;
      }
      auto& node_resources = it->second;
      for (const auto r : nr.resources) {
        auto rid = config.resourceNames.lookup(r.name);
        if (rid == StringTable::NotFound || rid >= node_resources.size()) {
          LOG(ERROR) << error.report(
            "Resource ", r.name, "/", rid, " not valid or known for node ",
            nr.node, ": ", debugString(rt)
          );
          continue;
        }
        node_resources[rid] -= r.amount;
        if (node_resources[rid] < 0) {
          LOG(ERROR) << error.report(
            "Resource ", r.name, " is ", node_resources[rid], " on node ",
            nr.node, " for ", debugString(rt)
          );
        }
      }
    }
  }
  if (FLAGS_log_performance) {
    timer.log("Running tasks computed");
  }

  // Detect orphans: running tasks whose jobs or nodes are deleted or disabled.
  // After this pass, only orphans will be left in node_ids_to_tasks.
  for (const auto& node : *nodes) {
    if (!node->enabled()) {
      continue;  // Act as if the node was not in nodes.
    }
    auto node_it = node_id_to_tasks.find(node->id());
    if (node_it == node_id_to_tasks.end()) {
      continue;  // No running tasks on this node.
    }
    for (auto it = node_it->second.begin(); it != node_it->second.end();) {
      // Only keep running tasks, whose jobs are deleted or disabled.
      auto jobs_it = config.jobs.find(it->job);
      if (jobs_it != config.jobs.end() && jobs_it->second->isEnabled()) {
        it = node_it->second.erase(it);  // Not a job or node orphan. Delete.
      } else {
        ++it;
      }
    }
  }
  for (auto&& p : node_id_to_tasks) {
    for (auto&& rt : p.second) {
      result.orphanTasks_.emplace_back(std::move(rt));
    }
    p.second.clear();
  }
  if (FLAGS_log_performance && !result.orphanTasks_.empty()) {
    timer.log("Found ", result.orphanTasks_.size(), " orphan running tasks");
  }

  // Compute cross product of jobs x nodes.
  vector<JobWithNodes> job_with_nodes;
  auto shuffled_nodes(nodes->shuffled());
  for (const auto& job_pair : shuffled(config.jobs)) {
    const auto& job = job_pair.second;
    if (!job->isEnabled()) {
      continue;
    }

    job_with_nodes.emplace_back();
    auto& job_nodes = job_with_nodes.back();
    job_nodes.job = job;

    const auto row = status_snapshot.getRow(job->id());
    for (const auto& node : shuffled_nodes) {
      // A job runs on a single level of the node hierarchy.  This is
      // probably the cheapest, most effective check, so do it first.
      if (node->level() != job->levelForTasks()) {
        continue;
      }

      // Exclude running, finished, failed, and backed-off tasks
      // NB It's much cheaper to check isRunning() than runningTasks_.
      auto s = row.getPtr(node->id());
      // It's much cheaper to check isRunning() than runningTasks_.
      if (s && (s->isRunning() || s->isDone() ||
                s->isFailed() || s->isInBackoff(cur_time))) {
        continue;
      }

      if (job->shouldRunOn(*node) != Job::ShouldRun::Yes) {
        continue;
      }

      // Remove tasks whose dependents have not finished yet.
      if (!all_of(
        job->dependencies().cbegin(),
        job->dependencies().cend(),
        [&status_snapshot, &node](Job::ID id) {
          const TaskStatus* s = status_snapshot.getPtr(id, node->id());
          return s && s->isDone();
        })) {
        continue;
      }

      job_nodes.nodes.push_back(node);
    }
  }

  if (FLAGS_log_performance) {
    timer.log("Cross product computed");
  }

  int scheduled_tasks =
    SchedulerPolicy::getSingleton(config.schedulerType)->schedule(
      job_with_nodes, resources_by_node, cb
    );

  if (scheduled_tasks) {
    timer.log("Scheduled ", scheduled_tasks, " tasks");
  }

  result.areTasksRunning_ = !running_tasks.empty() || (scheduled_tasks > 0);
  return result;
}

}}
