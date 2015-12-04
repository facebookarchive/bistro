/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "bistro/bistro/scheduler/SchedulerPolicy.h"

namespace facebook { namespace bistro {

class RoundRobinSchedulerPolicy : public SchedulerPolicy {
public:
  int schedule(std::vector<JobWithNodes>&, TaskRunnerCallback) override;
};

}}
