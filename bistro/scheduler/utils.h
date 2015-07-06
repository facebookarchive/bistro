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
