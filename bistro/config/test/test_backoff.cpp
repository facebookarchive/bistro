/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/config/JobBackoffSettings.h"

using namespace facebook::bistro;
using folly::dynamic;

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

template <typename... Args>
JobBackoffSettings make(Args&&... args) {
  return JobBackoffSettings(dynamic::array(std::forward<Args>(args)...));
}

TEST(TestJobBackoffSettings, HandleNonPositiveInterval) {
  EXPECT_THROW(make(0), std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleInvalidType) {
  EXPECT_THROW(make(1, 2, 3, "invalid"), std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleNonArray) {
  EXPECT_THROW({
    JobBackoffSettings jbs(dynamic::object());
  }, std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleNonInt) {
  EXPECT_THROW(make(1, 2, dynamic::object()), std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleDuplicate) {
  EXPECT_THROW(make(1, 1, 2, "fail"), std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleNoValues) {
  EXPECT_THROW({
    JobBackoffSettings jbs(dynamic::array());
  }, std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleInvalidRepeat) {
  EXPECT_THROW(make("repeat"), std::runtime_error);
}

TEST(TestJobBackoffSettings, HandleDirectFail) {
  auto s = make("fail");
  EXPECT_EQ(bd(-1), s.getNext(bd(0)));
}

TEST(TestJobBackoffSettings, HandleNoType) {
  auto s = make(1, 2, 3);
  EXPECT_EQ(bd(1), s.getNext(bd(0)));
  // We expect to default to fail
  EXPECT_EQ(bd(2), s.getNext(bd(1)));
  EXPECT_EQ(bd(3), s.getNext(bd(2)));
  EXPECT_EQ(bd(-1), s.getNext(bd(3)));
}

TEST(TestJobBackoffSettings, HandleFail) {
  auto s = make(1, 2, 3, "fail");
  EXPECT_EQ(bd(1), s.getNext(bd(0)));
  EXPECT_EQ(bd(2), s.getNext(bd(1)));
  EXPECT_EQ(bd(3), s.getNext(bd(2)));
  EXPECT_EQ(bd(-1), s.getNext(bd(3)));
}

TEST(TestJobBackoffSettings, HandleRepeat) {
  auto s = make(1, 2, 3, "repeat");
  EXPECT_EQ(bd(1), s.getNext(bd(0)));
  EXPECT_EQ(bd(2), s.getNext(bd(1)));
  EXPECT_EQ(bd(3), s.getNext(bd(2)));
  EXPECT_EQ(bd(3), s.getNext(bd(3)));
}

TEST(TestJobBackoffSettings, HandleConversion) {
  auto s = make(1, 2, 3, "repeat");
  auto s2 = JobBackoffSettings(s.toDynamic());
  EXPECT_EQ(s, s2);
}
