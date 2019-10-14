/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/scheduler/RandomizedPrioritySchedulerPolicy.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

TEST(TestRandomizedPriorityScheduler, HandleAll) {
  // Each trial should schedule 4 jobs, and since job 1 has priority of 10x that
  // of job2, we expect it to get scheduled about 10x more.
  const int runs = 100;
  map<string, int> job_count;
  for (int trial = 0; trial < runs; ++trial) {
    TaskCatcher catcher = mock_schedule<RandomizedPrioritySchedulerPolicy>();
    ASSERT_EQ(4, catcher.numTasks);
    ASSERT_EQ(4, catcher.tasks.size());
    for (const auto& pair : catcher.tasks) {
      ++job_count[pair.first];
    }
  }

  ASSERT_EQ(4 * runs, job_count["job1"] + job_count["job2"]);
  ASSERT_NEAR(4 * runs * 0.9, job_count["job1"], 40);
  // the probability of this going wrong is pow(.9, 400) = 5e-19
  ASSERT_GT(job_count["job2"], 0);
}
