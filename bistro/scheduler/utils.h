/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
