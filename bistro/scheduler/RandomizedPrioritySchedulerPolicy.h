/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/scheduler/SchedulerPolicy.h"

namespace facebook { namespace bistro {

class RandomizedPrioritySchedulerPolicy : public SchedulerPolicy {
public:
  int schedule(std::vector<JobWithNodes>&, TaskRunnerCallback) override;
};

}}
