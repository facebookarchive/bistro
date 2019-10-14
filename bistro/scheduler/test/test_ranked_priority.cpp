/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/scheduler/RankedPrioritySchedulerPolicy.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

TEST(TestRankedPriorityScheduler, HandleAll) {
  TaskCatcher catcher = mock_schedule<RankedPrioritySchedulerPolicy>(true);
  ASSERT_EQ(4, catcher.numTasks);
  ASSERT_EQ(4, catcher.tasks.size());

  // Job 3 has the highest priority but only wants to run on 1 node. We expect
  // job 1, which has the second highest priority, to win the rest.
  EXPECT_EQ((make_pair<string, string>("job3", "db12")), catcher.tasks[0]);
  EXPECT_EQ((make_pair<string, string>("job1", "db22")), catcher.tasks[1]);
  EXPECT_EQ((make_pair<string, string>("job1", "db21")), catcher.tasks[2]);
  EXPECT_EQ((make_pair<string, string>("job1", "db11")), catcher.tasks[3]);
}
