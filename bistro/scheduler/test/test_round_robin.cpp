/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/scheduler/RoundRobinSchedulerPolicy.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

TEST(TestRoundRobinScheduler, HandleAll) {
  TaskCatcher catcher = mock_schedule<RoundRobinSchedulerPolicy>();
  ASSERT_EQ(4, catcher.numTasks);
  ASSERT_EQ(4, catcher.tasks.size());

  // The round robin scheduler is deterministic (but schedules nodes from the
  // end of the list to be efficient), so we expect job1 to schedule
  // on the last node, then job2 on the penultimate node, etc.
  EXPECT_EQ((make_pair<string, string>("job1", "db22")), catcher.tasks[0]);
  EXPECT_EQ((make_pair<string, string>("job2", "db21")), catcher.tasks[1]);
  EXPECT_EQ((make_pair<string, string>("job1", "db12")), catcher.tasks[2]);
  EXPECT_EQ((make_pair<string, string>("job2", "db11")), catcher.tasks[3]);
}
