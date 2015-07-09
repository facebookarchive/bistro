/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include "bistro/bistro/config/JobBackoffSettings.h"

using namespace std;
using namespace facebook::bistro;
using namespace folly;

cpp2::BackoffDuration bd(int i) {
  cpp2::BackoffDuration d;
  if (i == -1) {
    d.noMoreBackoffs = true;
    d.seconds = 60;  // Hardcoded in JobBackoffSettings.cpp
  } else {
    d.noMoreBackoffs = false;
    d.seconds = i;
  }
  return d;
}

JobBackoffSettings make(const dynamic& d) {
  return JobBackoffSettings(d);
}

TEST(TestJobBackoffSettings, HandleNonPositiveInterval) {
  dynamic d = { 0 };
  EXPECT_THROW(make(d), runtime_error);
}

TEST(TestJobBackoffSettings, HandleInvalidType) {
  dynamic d = { 1, 2, 3, "invalid" };
  EXPECT_THROW(make(d), runtime_error);
}

TEST(TestJobBackoffSettings, HandleNonArray) {
  EXPECT_THROW(make(dynamic::object()), runtime_error);
}

TEST(TestJobBackoffSettings, HandleNonInt) {
  dynamic d = { 1, 2, dynamic::object };
  EXPECT_THROW(make(d), runtime_error);
}

TEST(TestJobBackoffSettings, HandleDuplicate) {
  dynamic d = { 1, 1, 2, "fail" };
  EXPECT_THROW(make(d), runtime_error);
}

TEST(TestJobBackoffSettings, HandleNoValues) {
  vector<int> v;
  dynamic d(v.begin(), v.end());
  EXPECT_THROW(make(d), runtime_error);
}

TEST(TestJobBackoffSettings, HandleInvalidRepeat) {
  dynamic d = { "repeat" };
  EXPECT_THROW(make(d), runtime_error);
}

TEST(TestJobBackoffSettings, HandleDirectFail) {
  auto s = make({ "fail" });
  EXPECT_EQ(bd(-1), s.getNext(bd(0)));
}

TEST(TestJobBackoffSettings, HandleNoType) {
  auto s = make({ 1, 2, 3 });
  EXPECT_EQ(bd(1), s.getNext(bd(0)));
  // We expect to default to fail
  EXPECT_EQ(bd(2), s.getNext(bd(1)));
  EXPECT_EQ(bd(3), s.getNext(bd(2)));
  EXPECT_EQ(bd(-1), s.getNext(bd(3)));
}

TEST(TestJobBackoffSettings, HandleFail) {
  auto s = make({1, 2, 3, "fail"});
  EXPECT_EQ(bd(1), s.getNext(bd(0)));
  EXPECT_EQ(bd(2), s.getNext(bd(1)));
  EXPECT_EQ(bd(3), s.getNext(bd(2)));
  EXPECT_EQ(bd(-1), s.getNext(bd(3)));
}

TEST(TestJobBackoffSettings, HandleRepeat) {
  auto s = make({1, 2, 3, "repeat"});
  EXPECT_EQ(bd(1), s.getNext(bd(0)));
  EXPECT_EQ(bd(2), s.getNext(bd(1)));
  EXPECT_EQ(bd(3), s.getNext(bd(2)));
  EXPECT_EQ(bd(3), s.getNext(bd(3)));
}

TEST(TestJobBackoffSettings, HandleConversion) {
  auto s = make({1, 2, 3, "repeat"});
  auto d = s.toDynamic();
  auto s2 = make(d);
  EXPECT_EQ(s, s2);
}
