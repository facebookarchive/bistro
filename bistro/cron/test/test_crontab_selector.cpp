/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/cron/CrontabSelector.h"
#include <folly/json.h>

using namespace facebook::bistro::detail_cron;
using namespace folly;
using namespace std;

/**
 * There are 7 clowns in the world, numbered from 1 through 7, with the
 * names listed below.
 */
CrontabSelector clown_selector(const string& json) {
  return CrontabSelector(parseJson(json), 1, 7, [](const string& s) {
    if (s == "sun" || s == "sunny") {
      return 1;
    } else if (s == "mon" || s == "monica") {
      return 2;
    } else if (s == "tue" || s == "tuelly") {
      return 3;
    } else if (s == "wed" || s == "wedder") {
      return 4;
    } else if (s == "thu" || s == "thurmond") {
      return 5;
    } else if (s == "fri" || s == "friedrich") {
      return 6;
    } else if (s == "sat" || s == "satay") {
      return 7;
    }
    throw std::runtime_error("Unknown string value " + s);
  });
}

string parse_to_str(const string& json) {
  return clown_selector(json).getPrintable();
}

TEST(TestCrontabSelector, CheckParseRanges) {
  EXPECT_EQ("*", parse_to_str("{}"));
  EXPECT_EQ("1-7:3", parse_to_str("{\"period\": 3}"));
  EXPECT_EQ("2-7:1", parse_to_str("{\"start\": 2}"));
  EXPECT_EQ("*", parse_to_str("{\"start\": \"Sunny\"}"));
  EXPECT_EQ("1-5:1", parse_to_str("{\"end\": \"Thu\"}"));
  EXPECT_EQ(
    "3-4:1", parse_to_str("{\"start\": \"Tuelly\", \"end\": \"Wed\"}")
  );
  EXPECT_EQ("1-6:2", parse_to_str("{\"end\": \"Fri\", \"period\": 2}"));
}

TEST(TestCrontabSelector, CheckParseValues) {
  EXPECT_EQ("5", parse_to_str("5"));
  EXPECT_EQ("5", parse_to_str("\"Thu\""));
  EXPECT_EQ("1,6,7", parse_to_str("[\"Sat\", 1, \"Fri\"]"));
}
TEST(TestCrontabSelector, CheckParseErrors) {
  // Out-of-range periods:
  EXPECT_THROW(clown_selector("{\"period\": 0}"), runtime_error);
  EXPECT_THROW(clown_selector("{\"period\": -1}"), runtime_error);
  EXPECT_THROW(clown_selector("{\"period\": 7}"), runtime_error);

  // Out-of-range start or end:
  EXPECT_THROW(clown_selector("{\"start\": 0}"), runtime_error);
  EXPECT_THROW(clown_selector("{\"end\": 8}"), runtime_error);

  // Problematic JSON inputs:
  EXPECT_THROW(clown_selector("{\"bad_key\": 3}"), runtime_error);
  EXPECT_THROW(clown_selector("0.1"), runtime_error);  // no floats

  // Invalid values:
  EXPECT_THROW(clown_selector("\"Chicken\""), runtime_error);
  EXPECT_THROW(clown_selector("\"Th\""), runtime_error);
  EXPECT_THROW(clown_selector("\"S\""), runtime_error);
  // no floats
  EXPECT_THROW(clown_selector("{\"start\": 0.3}"), runtime_error);
}

pair<int64_t, bool> p(int64_t t, bool carry) {
  return {t, carry};
}

TEST(TestCrontabSelector, CheckMatchDefaultRange) {
  auto obj = clown_selector("{}");
  EXPECT_EQ(p(1, false), obj.findFirstMatch(0));
  EXPECT_EQ(p(7, false), obj.findFirstMatch(7));
  EXPECT_EQ(p(1, true), obj.findFirstMatch(8));

  auto arr = clown_selector("{}");
  EXPECT_EQ(p(1, false), obj.findFirstMatch(1));
  EXPECT_EQ(p(6, false), obj.findFirstMatch(6));
  EXPECT_EQ(p(1, true), obj.findFirstMatch(10));
}

TEST(TestCrontabSelector, CheckMatchValues) {
  auto five = clown_selector("5");
  for (int64_t i = 0; i < 10; ++i) {  // A single value never changes
    EXPECT_EQ(p(5, i > 5), five.findFirstMatch(i));
  }

  // Now check a list of values
  auto arr = clown_selector("[2,4,5]");
  EXPECT_EQ(p(2, false), arr.findFirstMatch(0));
  EXPECT_EQ(p(2, false), arr.findFirstMatch(1));
  EXPECT_EQ(p(2, false), arr.findFirstMatch(2));
  EXPECT_EQ(p(4, false), arr.findFirstMatch(3));
  EXPECT_EQ(p(4, false), arr.findFirstMatch(4));
  EXPECT_EQ(p(5, false), arr.findFirstMatch(5));
  EXPECT_EQ(p(2, true), arr.findFirstMatch(6));
  EXPECT_EQ(p(2, true), arr.findFirstMatch(7));
}

TEST(TestCrontabSelector, CheckMatchNontrivialRange) {
  auto range = clown_selector("{\"start\": 3, \"end\": 7, \"period\": 2}");
  EXPECT_EQ(p(3, false), range.findFirstMatch(1));
  EXPECT_EQ(p(3, false), range.findFirstMatch(2));
  EXPECT_EQ(p(3, false), range.findFirstMatch(3));
  EXPECT_EQ(p(5, false), range.findFirstMatch(4));
  EXPECT_EQ(p(5, false), range.findFirstMatch(5));
  EXPECT_EQ(p(7, false), range.findFirstMatch(6));
  EXPECT_EQ(p(7, false), range.findFirstMatch(7));
  EXPECT_EQ(p(3, true), range.findFirstMatch(8));
}
