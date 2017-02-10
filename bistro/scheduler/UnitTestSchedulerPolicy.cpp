/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/UnitTestSchedulerPolicy.h"

#include <folly/Singleton.h>

namespace facebook { namespace bistro {

// Scheduler policies are ephemeral, so they get the callback via a singleton.
namespace {
  folly::Singleton<UnitTestSchedulerPolicy::TestPolicyCob> test_policy_cob;
}

int UnitTestSchedulerPolicy::schedule(
    std::vector<JobWithNodes>& jobs,
    TaskRunnerCallback cb) {
  return test_policy_cob.try_get()->operator()(jobs, cb);
}

/* static */ std::shared_ptr<UnitTestSchedulerPolicy::TestPolicyCob>
UnitTestSchedulerPolicy::testPolicyCob() {
  return test_policy_cob.try_get();
}

}}
