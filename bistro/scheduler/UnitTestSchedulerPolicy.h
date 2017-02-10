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

/**
 * A mock that allows us to test the scheduler. NOT THREAD-SAFE!
 *
 * To use, put this **before** calling Scheduler::schedule():
 *   // Must survive until after the Scheduler::schedule() call!
 *   auto cob_ptr = UnitTestSchedulerPolicy::testPolicyCob();
 *   *cob_ptr = [](...) {};
 */
struct UnitTestSchedulerPolicy : public SchedulerPolicy {
  using TestPolicyCob =
    std::function<int(std::vector<JobWithNodes>&, TaskRunnerCallback)>;
  int schedule(std::vector<JobWithNodes>& j, TaskRunnerCallback) override;
  // Stored using a singleton since policies themselves are ephemeral.  You
  // must keep this shared_ptr alive from **before** your test invokes the
  // scheduler, until **just after** you no longer need it.
  static std::shared_ptr<TestPolicyCob> testPolicyCob();
};

}}
