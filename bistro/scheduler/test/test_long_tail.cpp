/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/scheduler/test/utils.h"
#include "bistro/bistro/scheduler/LongTailSchedulerPolicy.h"

using namespace folly;
using namespace std;
using namespace facebook::bistro;

TEST(TestLongTailScheduler, HandleAll) {
  TaskCatcher catcher = mock_schedule<LongTailSchedulerPolicy>(true);
  ASSERT_EQ(4, catcher.numTasks);
  ASSERT_EQ(4, catcher.tasks.size());

  // Job 3 has the fewest remaining tasks, thus we prefer it above the other
  // jobs.
  EXPECT_EQ((make_pair<string, string>("job3", "db12")), catcher.tasks[0]);

  // The remaining jobs have the same number of tasks. We don't care about the
  // order in that case. One of them should win, but it's arbitrary.
  string job = catcher.tasks[1].first;
  EXPECT_EQ(job, catcher.tasks[2].first);
  EXPECT_EQ(job, catcher.tasks[3].first);

  // Also check that we got all the dbs (we expect them in reverse order).
  EXPECT_EQ("db22", catcher.tasks[1].second);
  EXPECT_EQ("db21", catcher.tasks[2].second);
  EXPECT_EQ("db11", catcher.tasks[3].second);
}
