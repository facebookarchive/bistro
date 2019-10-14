/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/scheduler/Scheduler.h"

#include <folly/experimental/AutoTimer.h>
#include <list>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/flags/Flags.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/scheduler/SchedulerPolicy.h"
#include "bistro/bistro/scheduler/SchedulerPolicyRegistry.h"
#include "bistro/bistro/statuses/TaskStatusSnapshot.h"
#include "bistro/bistro/utils/ShuffledRange.h"

namespace facebook { namespace bistro {

using apache::thrift::debugString;

namespace {
// Lets the NodeGroup member be const.
std::unordered_map<std::string, size_t> makeResourceToIndex(
    const Config& config, int ngid) {
  std::unordered_map<std::string, size_t> resource_to_index;
  // The analogous loop in JobWithNodes must produce matching indices.
  const auto& rids = config.levelIDToResourceID[ngid];
  for (size_t i = 0; i < rids.size(); ++i) {
    resource_to_index[config.resourceNames.lookup(rids[i])] = i;
  }
  return resource_to_index;
}

// Collects nodes of one logical type, and their resources.
// This used to be known as `level`.
struct NodeGroup {
  NodeGroup(const Config& config, int id)
    : nodeGroupID_(id), resourceToIndex_(makeResourceToIndex(config, id)) {}

  // Called after nodes_ is no longer growing.
  void packResourcesInto(const Config& c, NodeGroupToPackedResources* ngpr) {
    const auto& rid_to_amount = c.resourcesByLevel.at(nodeGroupID_);
    const auto& rids = c.levelIDToResourceID.at(nodeGroupID_);
    auto& packed_resources = (*ngpr)[nodeGroupID_];
    std::vector<int> amounts;
    amounts.reserve(rids.size());
    // The resource indices here must match those in JobWithNodes and in
    // resourceToIndex_.
    for (const auto& rid : rids) {
      auto amount = rid_to_amount.at(rid);
      CHECK_NE(std::numeric_limits<int>::max(), amount);  // Config's sentinel
      amounts.push_back(amount);
    }
    // Repeat the amounts for each node
    packed_resources.reserve(amounts.size() * nodes_.size());
    // Replicas are a hack: multiple nodes with the same name and different
    // parents.  They have to share the same packed resource, since they
    // represent the same logical piece of work.  Future: model replicas as
    // follows: runs on "db1_host1" -> uses "db1" and "host1" resources.
    std::unordered_map<std::string, size_t> node_name_to_offset;
    node_name_to_offset.reserve(nodes_.size());
    for (auto* node : nodes_) {
      // The node's offset into this NodeGroup's PackedResources is assigned
      // by the first replica, and reused for all additional ones.
      //
      // This is done **after** reordering to make deterministic tests easier.
      auto offset_insert =
        node_name_to_offset.emplace(node->name(), node_name_to_offset.size());
      node->offset = offset_insert.first->second * amounts.size();
      // If this is a new offset, add packed resources for it.
      if (offset_insert.second) {
        packed_resources.insert(
          packed_resources.end(), amounts.begin(), amounts.end()
        );
      }
    }
    CHECK_EQ(
      node_name_to_offset.size() * amounts.size(),
      packed_resources.size()
    );
  }

  void reorderNodes(NodeOrderType order) {
    switch (order) {
      case NodeOrderType::Original:
        break;  // Nothing to do
      case NodeOrderType::Lexicographic:
        std::sort(
          nodes_.begin(), nodes_.end(), [&] (const Node* a, const Node* b) {
            return a->name() < b->name();
          }
        );
        break;
      case NodeOrderType::Random:
        std::shuffle(nodes_.begin(), nodes_.end(), folly::ThreadLocalPRNG());
        break;
      default:
        LOG(FATAL) << "Unknown node_order: " << static_cast<int>(order);
    }
  }

  const int nodeGroupID_;  // Formerly `level`, will be removed in the future.
  // Each node in a NodeGroup has the same allowed resources. The names of
  // these resources map to sequential 0-based indexes, used below.
  const std::unordered_map<std::string, size_t> resourceToIndex_;
  // Nodes just as fetched by the NodeFetcher, but their mutable `offset`
  // field got set to point into PackedResources.
  std::vector<const Node*> nodes_;
};

void processRunningTasks(
    const Config& config,
    Monitor* monitor,
    const std::unordered_map<std::pair<Job::ID, Node::ID>, cpp2::RunningTask>&
      running_tasks,
    const std::unordered_map<int, NodeGroup>& node_groups,
    std::vector<cpp2::RunningTask>* orphan_tasks,
    NodeGroupToPackedResources* node_group_packed_resources) {
  DEFINE_MONITOR_ERROR(monitor, error, "Running task resources");

  // Build two temporary views on running_tasks (pointers, not copies).
  //
  // Find node "orphans": running tasks, whose nodes are deleted/disabled.
  // Use 'list' for stable iterators, so orphan search can erase easily.
  std::unordered_map<std::string, std::list<const cpp2::RunningTask*>>
    node_to_tasks;
  // Used to subtract out node resources consumed by running tasks.
  std::unordered_map<std::string, std::vector<const cpp2::Resource*>>
    node_used_resources;
  for (const auto& id_and_task : running_tasks) {
    const auto& rt = id_and_task.second;
    node_to_tasks[id_and_task.second.node].push_front(&id_and_task.second);
    for (const auto& nr : rt.nodeResources) {
      auto& resources = node_used_resources[nr.node];
      for (const auto& res : nr.resources) {
        resources.push_back(&res);
      }
    }
  }

  // Subtract out resources used by running tasks from
  // node_group_packed_resources, and simultaneously detect orphans (running
  // tasks whose jobs or nodes are deleted or disabled).  After this pass,
  // only orphans will be left in node_to_tasks.
  //
  // With replica nodes, a task is considered an orphan only when all the
  // replicas go down.  This isn't really the right behavior, but it's not
  // so broken as to fix urgently.  Future: model replicas as follows: runs
  // on "db1_host1" -> uses "db1" and "host1" resources.
  for (auto& lng : node_groups) {
    PackedResources& packed_resources =
      (*node_group_packed_resources)[lng.first];
    for (const auto& node : lng.second.nodes_) {
      // Find orphans: keep only tasks, whose jobs are deleted or disabled.
      if (node->enabled()) {  // For orphans, disabled is the same as deleted.
        auto tasks = node_to_tasks.find(node->name());
        if (tasks != node_to_tasks.end()) {  // Does node have running tasks?
          for (auto it = tasks->second.begin(); it != tasks->second.end();) {
            auto jobs_it = config.jobs.find((*it)->job);
            if (jobs_it != config.jobs.end() && jobs_it->second->canRun()) {
              it = tasks->second.erase(it);  // Not a job or node orphan.
            } else {
              ++it;  // Leave orphans in the list.
            }
          }
        }
      }
      // Subtract out resources used by the running tasks. We do this in a
      // outside of the cross-product loop, because:
      //  - This way, we account even for tasks from deleted jobs
      //  - running_tasks.find() for every job-node combination is expensive
      // Note that we don't care if the task's node is disabled -- the task
      // is running, and may be using resources on non-disabled nodes.
      auto resources_it = node_used_resources.find(node->name());
      if (resources_it != node_used_resources.end()) {
        for (const cpp2::Resource* r : resources_it->second) {
          auto rsrc_it = lng.second.resourceToIndex_.find(r->name);
          if (rsrc_it == lng.second.resourceToIndex_.end()) {
            LOG(ERROR) << error.report(
              "Resource ", r->name, " not found in the node-group of node ",
              node->name(), ": ", debugString(*r)
            );
            // We log, and ignore the bad resource as if it didn't exist.
            // Don't try to kill such tasks, since it can be easy to
            // transiently misconfigure resources such that this fires.
            continue;
          }
          auto& amount = packed_resources.at(node->offset + rsrc_it->second);
          amount -= r->amount;
          if (amount < 0) {
            LOG(ERROR) << error.report(
              "Resource ", r->name, " is ", amount, " on node ", node->name(),
              " for ", debugString(*r)
            );
          }
        }
        // Delete the node now that we accounted for its resources.  That
        // way, at the end of the loop, only unknown nodes will remain in
        // node_used_resources, enabling the warning loop below.
        node_used_resources.erase(resources_it);
      }
    }
  }
  // Copy the orphan running tasks -- Scheduler will return these.
  for (const auto& p : node_to_tasks) {
    for (const auto& rt : p.second) {
      orphan_tasks->emplace_back(*rt);
    }
  }
  // HACK HACK HACK: Until workers become nodes, we cannot warn about
  // unknown worker nodes.  Since we don't have a list of worker node names
  // handy, identify these nodes by their resource names.
  std::unordered_set<std::string> worker_resource_names;
  auto worker_level = config.levels.lookup("worker");
  CHECK_GE(worker_level, 0);
  CHECK_LT(worker_level, config.levelIDToResourceID.size());
  for (auto rid : config.levelIDToResourceID[worker_level]) {
    worker_resource_names.emplace(config.resourceNames.lookup(rid));
  }
  // Warn about nodes mentioned by running tasks, which we don't know about.
  for (const auto& p : node_used_resources) {
    std::string rs;
    size_t num_worker_resources = 0;
    for (const cpp2::Resource* r : p.second) {
      num_worker_resources += worker_resource_names.count(r->name);
      rs += debugString(*r) + "\n";
    }
    if (num_worker_resources != 0) {
      if (num_worker_resources == p.second.size()) {
        continue;  // We don't know about workers in this loop yet.
      }
      rs += "[ERROR: Not all node resources were worker resources]\n";  // WTF?
    }
    LOG(ERROR) << error.report(
      "Running task requires resources on unknown node ", p.first, ":\n", rs
    );
  }
}
}  // anonymous namespace

Scheduler::Result Scheduler::schedule(
    time_t cur_time,
    const Config& config,
    const std::shared_ptr<const Nodes>& nodes,
    const TaskStatusSnapshot& status_snapshot,
    TaskRunnerCallback cb,
    const std::shared_ptr<Monitor> monitor) {

  folly::AutoTimer<> timer;

  CHECK(config.levelIDToResourceID.size() == config.resourcesByLevel.size());

  // Here and below, level == NodeGroup. Future: rename 'level' throughout.
  //
  // Group nodes & resources by their NodeGroup -- this speeds up and
  // simplifies the rest of the scheduling computation
  std::unordered_map<int, NodeGroup> node_groups;
  for (int ngid = 0; ngid < config.levelIDToResourceID.size(); ++ngid) {
    node_groups.emplace(ngid, NodeGroup(config, ngid));
  }
  for (const auto& node : *nodes) {
    node_groups.at(node->level()).nodes_.push_back(node.get());
  }
  if (FLAGS_log_performance) {
    timer.log("Did precomputations that will be eliminated by a refactor");
  }

  NodeGroupToPackedResources node_group_packed_resources;
  for (auto& lng : node_groups) {
    // Reorder before packing, since that makes deterministic tests easier.
    lng.second.reorderNodes(config.nodeOrderType);
    lng.second.packResourcesInto(config, &node_group_packed_resources);
  }
  if (FLAGS_log_performance) {
    timer.log("Prepared resource vectors");
  }

  // Identify orphaned tasks, and subtract running tasks' resources from
  // node_group_packed_resources.
  Scheduler::Result result;
  processRunningTasks(
    config,
    monitor.get(),
    status_snapshot.getRunningTasks(),
    node_groups,
    &result.orphanTasks_,
    &node_group_packed_resources
  );
  if (FLAGS_log_performance) {
    timer.log("Accounted for running task resources");
  }
  if (!result.orphanTasks_.empty()) {
    timer.log("Found ", result.orphanTasks_.size(), " orphan running tasks");
  }

  // Compute cross product of jobs x nodes.
  std::vector<JobWithNodes> job_with_nodes;
  for (const auto& job_pair : shuffled(config.jobs)) {
    const auto& job = job_pair.second;
    if (!job->canRun()) {
      continue;
    }

    job_with_nodes.emplace_back(config, job, &node_group_packed_resources);
    auto& job_nodes = job_with_nodes.back();

    const auto row = status_snapshot.getRow(job->id());
    auto ng_it = node_groups.find(job->levelForTasks());
    if (ng_it == node_groups.end()) {
      continue;
    }
    for (const auto& node : ng_it->second.nodes_) {
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
      if (!std::all_of(
        job->dependencies().cbegin(),
        job->dependencies().cend(),
        [&status_snapshot, &node](Job::ID id) {
          const TaskStatus* st = status_snapshot.getPtr(id, node->id());
          return st && st->isDone();
        })) {
        continue;
      }

      job_nodes.nodes.push_back(node);
    }
  }
  if (FLAGS_log_performance) {
    timer.log("Cross product computed");
  }

  int scheduled_tasks = getSchedulerPolicy(config.schedulerPolicyName)
    ->schedule(job_with_nodes, cb);
  if (scheduled_tasks) {
    timer.log("Scheduled ", scheduled_tasks, " tasks");
  }

  result.areTasksRunning_ =
    !status_snapshot.getRunningTasks().empty() || (scheduled_tasks > 0);
  return result;
}

}}
