/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/cron/EpochCrontabItem.h"

#include <folly/dynamic.h>
#include <folly/json.h>

using namespace facebook::bistro::detail_cron;
using namespace facebook::bistro;
using namespace folly;
using namespace std;


unique_ptr<const CrontabItem> makeEpochCrontabItem(const string& json) {
  // A null timezone is enough since EpochCrontabItem does not look at it.
  boost::local_time::time_zone_ptr tz;
  return CrontabItem::fromDynamic(
    dynamic::object("epoch", parseJson(json)), tz
  );
}

// This test is pretty redundant with the CrontabSelector test, but it was
// easy to add.

TEST(TestEpochCrontabItem, SingleValue) {
  auto ci = makeEpochCrontabItem("500");
  EXPECT_EQ(500, ci->findFirstMatch(0).value());
  EXPECT_EQ(500, ci->findFirstMatch(499).value());
  EXPECT_EQ(500, ci->findFirstMatch(500).value());
  EXPECT_FALSE(ci->findFirstMatch(501).has_value());
  EXPECT_FALSE(ci->findFirstMatch(2000000000).has_value());
}

TEST(TestEpochCrontabItem, Values) {
  auto ci = makeEpochCrontabItem("[500, 7000000]");
  EXPECT_EQ(500, ci->findFirstMatch(0).value());
  EXPECT_EQ(500, ci->findFirstMatch(500).value());
  EXPECT_EQ(7000000, ci->findFirstMatch(501).value());
  EXPECT_FALSE(ci->findFirstMatch(7000001).has_value());
}

TEST(TestEpochCrontabItem, Interval) {
  auto ci = makeEpochCrontabItem("{\"start\": 5000, \"period\": \"500\"}");
  EXPECT_EQ(5000, ci->findFirstMatch(0).value());
  EXPECT_EQ(5000, ci->findFirstMatch(5000).value());
  EXPECT_EQ(5500, ci->findFirstMatch(5001).value());
  EXPECT_EQ(6000, ci->findFirstMatch(5623).value());
  EXPECT_EQ(1234568000, ci->findFirstMatch(1234567890).value());
  EXPECT_EQ(1234568500, ci->findFirstMatch(1234568123).value());
}

TEST(TestEpochCrontabItem, StartOnly) {
  auto ci = makeEpochCrontabItem("{\"start\": 750, \"period\": 300}");
  EXPECT_EQ(750, ci->findFirstMatch(3).value());
  EXPECT_EQ(92885850, ci->findFirstMatch(92885829).value());
  EXPECT_EQ(92885850, ci->findFirstMatch(92885850).value());
  EXPECT_EQ(92886150, ci->findFirstMatch(92885851).value());
  EXPECT_EQ(92886150, ci->findFirstMatch(92886150).value());
}

TEST(TestEpochCrontabSelector, EndOnly) {
  auto ci = makeEpochCrontabItem("{\"end\": 1500, \"period\": 30}");
  EXPECT_EQ(0, ci->findFirstMatch(0).value());
  EXPECT_EQ(30, ci->findFirstMatch(1).value());
  EXPECT_EQ(1440, ci->findFirstMatch(1440).value());
  EXPECT_EQ(1470, ci->findFirstMatch(1441).value());
  EXPECT_EQ(1500, ci->findFirstMatch(1471).value());
  EXPECT_EQ(1500, ci->findFirstMatch(1500).value());
  EXPECT_FALSE(ci->findFirstMatch(1501).has_value());
}
