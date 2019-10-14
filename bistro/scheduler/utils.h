/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>
#include <unordered_map>

#include "bistro/bistro/scheduler/SchedulerPolicy.h"
#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Node;
class Job;
class JobWithNodes;

TaskRunnerResponse try_to_schedule(
  const Node&,
  const JobWithNodes&,
  TaskRunnerCallback
);

}}
