/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/date_time/local_time/local_time.hpp>  // for posix_time_zone

#include "bistro/bistro/cron/StandardCrontabItem.h"

#include <folly/dynamic.h>
#include <folly/json.h>

using namespace facebook::bistro::detail_cron;
using namespace facebook::bistro;
using namespace folly;
using namespace std;

// TODO: More tests here wouldn't hurt, e.g.
//  - Exercise some more "normal" timestamps
//  - Make sure that matches at the very edges of the DST-ambiguous intervals
//    work fine (I believe they do from inspecting the code's internal vars)
//  - Add some sort of randomized testing (good random selectors are hard,
//    though).
//  - Exercise timezones that aren't US Pacific.

// The DST-fix setup used in a majority of tests
const string kDstPrefix = "{\"dst_fixes\": [\"skip\", \"repeat_use_both\"], ";

// The 'default' harness we use for testing findFirstMatch -- assumes we match
int64_t def_match(
    const string &json,
    boost::local_time::time_zone_ptr tz,
    int64_t t) {

  return CrontabItem::fromDynamic(
    parseJson(kDstPrefix + json), tz
  )->findFirstMatch(t).value();
}

TEST(TestStandardCrontabItem, TestAll) {
  // Make the test deterministic by running in the US Pacific timezone, and
  // test both system and explicit timezone code paths.
  boost::local_time::time_zone_ptr explicit_tz{
    // The timezone is -8 here, but +8 below due to boost bug 3336
    new boost::local_time::posix_time_zone{"PST-8PDT,M3.2.0,M11.1.0"}
  };
  boost::local_time::time_zone_ptr local_tz;
  setenv("TZ", "PST+8PDT,M3.2.0,M11.1.0", 1);
  tzset();

  for (auto tz : {explicit_tz, local_tz}) {
    // Minute-only matches
    EXPECT_EQ(0, def_match("\"minute\": {}}", tz, 0));
    EXPECT_EQ(180, def_match("\"minute\": {\"start\": 3}}", tz, 0));

    auto min7 = CrontabItem::fromDynamic(
      parseJson(kDstPrefix + "\"minute\": {\"period\": 7}}"), tz
    );
    EXPECT_EQ(0, min7->findFirstMatch(0).value());
    EXPECT_EQ(7*60, min7->findFirstMatch(1).value());
    EXPECT_EQ(7*7*60, min7->findFirstMatch(7*7*60).value());
    EXPECT_EQ(8*7*60, min7->findFirstMatch(7*7*60 + 1).value());
    EXPECT_EQ(8*7*60, min7->findFirstMatch(8*7*60 - 1).value());
    EXPECT_EQ(8*7*60, min7->findFirstMatch(8*7*60).value());

    // Check wrapping behavior at the 1-hour mark
    EXPECT_EQ(3600, min7->findFirstMatch(8*7*60 + 1).value());
    EXPECT_EQ(3600 + 7*60, min7->findFirstMatch(3600 + 1).value());

    // Check end-of-day wrap
    auto eod = 28800;  // Thu Jan  1 00:00:00 PST 1970
    EXPECT_EQ(eod, min7->findFirstMatch(eod - 1).value());
    EXPECT_EQ(eod, min7->findFirstMatch(eod).value());
    EXPECT_EQ(eod + 7*60, min7->findFirstMatch(eod + 1).value());

    // Searching for the 31st of the month skips September
    EXPECT_EQ(
      215593200,  // Oct 31 PDT 1976 => Sep 12 PDT 1976
      def_match("\"minute\": {}, \"day_of_month\": [31]}", tz, 211410000)
    );

    EXPECT_EQ(  // Searching for Feb 29 in a leap year
      1204335420,  def_match(  // Fri Feb 29 17:37:00 PST 2008
        "\"minute\": [37], "
        "\"hour\": 17, "
        "\"day_of_month\": [29], "
        "\"month\": \"Feb\", "
        "\"year\": {\"start\": 1999, \"period\": 3}}", tz,
        1107214567  // Mon Jan 31 15:36:07 PST 2005
      )
    );

    // Since there is no Feb 29 in an odd-numbered year, this searches
    // through all available years, and fails due to carry.
    EXPECT_THROW(
      CrontabItem::fromDynamic(
        parseJson(kDstPrefix +
          "\"minute\": {}, "
          "\"day_of_month\": [29], "
          "\"month\": \"FEBRUARY\", "
          "\"year\": {\"start\": 2001, \"period\": 2}}"),
        tz
      )->findFirstMatch(0),
      runtime_error
    );

    //
    // Test day-of-week handling
    //

    // No DoW carry: Sun 19, Fri 24, Sat 25 => Sat Jan 25 00:00:00 PST 2014
    auto sat = CrontabItem::fromDynamic(
      parseJson(kDstPrefix + "\"minute\": {}, \"day_of_week\": 7}"), tz
    );
    for (auto t : {1390118400, 1390550400, 1390636800}) {
      EXPECT_EQ(1390636800, sat->findFirstMatch(t).value());
    }

    // Corner case: Carry from Tue May 16 09:20:18 PDT 2017 to the next Tue.
    {
      auto tue8am = CrontabItem::fromDynamic(parseJson(
        kDstPrefix + "\"hour\": 8, \"minute\": 50, \"day_of_week\": 3}"
      ), tz);
      EXPECT_EQ(1495554600, tue8am->findFirstMatch(1494951618).value());
    }

    // DoW carry: Mon 20, Sat 25, Sun 26 => Sun Jan 26 00:00:00 PST 2014
    auto sun = CrontabItem::fromDynamic(
      parseJson(kDstPrefix + "\"minute\": {}, \"day_of_week\": 1}"), tz
    );
    for (auto t : {1390204800, 1390636800, 1390723200}) {
      EXPECT_EQ(1390723200, sun->findFirstMatch(t).value());
    }

    EXPECT_EQ(  // No more Tuesdays in November, so go to March
      383904000, def_match(  // Tue Mar  2 00:00:00 PST 1982
        "\"minute\": {}, \"day_of_week\": 3, \"month\": [3, 11]}", tz,
        375523200  // Wed Nov 25 00:00:00 PST 1981
      )
    );

    EXPECT_EQ(  // No more Tuesdays in November, so go to March
      383904000,  def_match(  // Tue Mar  2 00:00:00 PST 1982
        "\"minute\": {}, \"day_of_week\": 3, \"month\": [3, 11]}", tz,
        375523200  // Wed Nov 25 00:00:00 PST 1981
      )
    );

    //
    // Test what happens when we match time labels that are skipped by
    // DST setting the clock forward.
    //
    time_t pre_skip_t = 1362909540; // Sun Mar 10 01:59:00 PST 2013
    // dst_fixes tell us to ignore the matching time labels that were skipped
    EXPECT_EQ(pre_skip_t + 4*60, def_match("\"minute\": 3}", tz, pre_skip_t));
    EXPECT_EQ(
      pre_skip_t + 4*60,
      def_match("\"minute\": [3, 7, 9]}", tz, pre_skip_t)
    );
    // "unskip" makes a synthetic match to make up for any skipped matches
    EXPECT_EQ(
      pre_skip_t + 59,  // "unskip" matches the second before the skip
      CrontabItem::fromDynamic(parseJson(
        "{\"dst_fixes\": [\"unskip\", \"repeat_use_both\"], \"minute\": 3}"
      ), tz)->findFirstMatch(pre_skip_t).value()
    );

    //
    // Test that we handle the time period when DST sets the clock backward.
    //
    time_t first_ambig = 1383465600;  // Sun Nov  3 01:00:00 PDT 2013
    time_t mid_ambig = first_ambig + 3600;
    time_t first_unambig = mid_ambig + 3600;

    auto use_both = CrontabItem::fromDynamic(parseJson(
      "{\"dst_fixes\": [\"skip\", \"repeat_use_both\"], \"minute\": [3, 7]}"
    ), tz);
    for (const auto& out_in : vector<vector<time_t>>{
      // "both" matches the early set of matches...
      {first_ambig + 3*60, first_ambig - 90},
      {first_ambig + 3*60, first_ambig},
      {first_ambig + 3*60, first_ambig + 3*60},
      {first_ambig + 7*60, first_ambig + 3*60 + 1},
      {first_ambig + 7*60, first_ambig + 7*60},
      {mid_ambig + 3*60, first_ambig + 7*60 + 1},
      // ...and also the late set of matches.
      {mid_ambig + 3*60, mid_ambig - 90},
      {mid_ambig + 3*60, mid_ambig},
      {mid_ambig + 3*60, mid_ambig + 3*60},
      {mid_ambig + 7*60, mid_ambig + 3*60 + 1},
      {mid_ambig + 7*60, mid_ambig + 7*60},
      {first_unambig + 3*60, mid_ambig + 7*60 + 1},
    }) {
      EXPECT_EQ(out_in[0], use_both->findFirstMatch(out_in[1]).value());
    }

    // "early" matches only the early set of matches
    auto use_early = CrontabItem::fromDynamic(parseJson(
      "{\"dst_fixes\": [\"skip\", \"repeat_use_only_early\"], "
      "\"minute\": [3, 7]}"
    ), tz);
    for (const auto& out_in : vector<vector<time_t>>{
      {first_ambig + 3*60, first_ambig - 90},
      {first_ambig + 3*60, first_ambig},
      {first_ambig + 3*60, first_ambig + 3*60},
      {first_ambig + 7*60, first_ambig + 3*60 + 1},
      {first_ambig + 7*60, first_ambig + 7*60},
      {first_unambig + 3*60, first_ambig + 7*60 + 1},
    }) {
      EXPECT_EQ(out_in[0], use_early->findFirstMatch(out_in[1]).value());
    }

    // "late" matches only the late set of matches
    auto use_late = CrontabItem::fromDynamic(parseJson(
      "{\"dst_fixes\": [\"skip\", \"repeat_use_only_late\"], "
      "\"minute\": [3, 7]}"
    ), tz);
    for (const auto& out_in : vector<vector<time_t>>{
      {mid_ambig + 3*60, first_ambig - 90},
      {mid_ambig + 3*60, mid_ambig - 90},
      {mid_ambig + 3*60, mid_ambig},
      {mid_ambig + 3*60, mid_ambig + 3*60},
      {mid_ambig + 7*60, mid_ambig + 3*60 + 1},
      {mid_ambig + 7*60, mid_ambig + 7*60},
      {first_unambig + 3*60, mid_ambig + 7*60 + 1},
    }) {
      EXPECT_EQ(out_in[0], use_late->findFirstMatch(out_in[1]).value());
    }

    auto two_years = CrontabItem::fromDynamic(parseJson(
      kDstPrefix +
      "\"year\": [2003, 2005], "
      "\"month\": 1, "
      "\"day_of_month\": 1, "
      "\"hour\": 0, "
      "\"minute\": 0}"
    ), tz);
    // 1970 => 2003
    EXPECT_EQ(1041408000, two_years->findFirstMatch(0).value());
    // 2003 + 1 second => 2005
    EXPECT_EQ(1104566400, two_years->findFirstMatch(1041408001).value());
    // 2005 + 1 second => no match
    EXPECT_FALSE(two_years->findFirstMatch(1104566401).has_value());
  }
}

// This attempts to cover all the runtime_errors (i.e. all usage errors).
TEST(TestStandardCrontabItem, TestErrors) {
  boost::local_time::time_zone_ptr tz;  // Timezone doesn't matter here
  // Cannot specify both day of month and day of week
  EXPECT_THROW(CrontabItem::fromDynamic(parseJson(kDstPrefix +
    "\"minute\": 0, \"day_of_month\": 1, \"day_of_week\": 1}"
  ), tz), runtime_error);
  // Unknown selector
  EXPECT_THROW(CrontabItem::fromDynamic(parseJson(kDstPrefix +
    "\"minute\": 0, \"clown\": 1}"
  ), tz), runtime_error);
  // Must specify a minute selector
  EXPECT_THROW(CrontabItem::fromDynamic(parseJson(
    "{\"dst_fixes\": [\"skip\", \"repeat_use_both\"]}"
  ), tz), runtime_error);

  // Incomplete or broken dst_fixes
  EXPECT_THROW(CrontabItem::fromDynamic(dynamic::object(), tz), runtime_error);
  EXPECT_THROW(
      CrontabItem::fromDynamic(
          dynamic::object("dst_fixes", folly::dynamic::array("unskip")), tz),
      runtime_error);
  EXPECT_THROW(
      CrontabItem::fromDynamic(
          dynamic::object(
              "dst_fixes", folly::dynamic::array("repeat_use_both")),
          tz),
      runtime_error);
  EXPECT_THROW(CrontabItem::fromDynamic(
    dynamic::object("dst_fixes", dynamic::object()), tz
  ), runtime_error);
  // Redundant DST fix
  EXPECT_THROW(
      CrontabItem::fromDynamic(
          dynamic::object("minute", 0)(
              "dst_fixes",
              folly::dynamic::array("skip", "repeat_use_both", "unskip")),
          tz),
      runtime_error);
  // Bad DST fix entry
  EXPECT_THROW(
      CrontabItem::fromDynamic(
          dynamic::object("minute", 0)(
              "dst_fixes",
              folly::dynamic::array("skip", "repeat_use_both", "clown")),
          tz),
      runtime_error);
}

string parse_to_str(const string& json) {
  boost::local_time::time_zone_ptr tz;  // Timezone doesn't matter here
  return CrontabItem::fromDynamic(parseJson(
    kDstPrefix + "\"minute\": {}, " + json
  ), tz)->getPrintable();
}

TEST(TestStandardCrontabItem, ParseMonthNames) {
  EXPECT_EQ("* * * 1,2,3,4,5,6 * *", parse_to_str(
    "\"month\": [\"January\", \"FEB\", \"march\", \"Apr\", \"May\", 6]}"
  ));
  EXPECT_EQ("* * * 6,7,8,9,10,11 * *", parse_to_str(
    "\"month\": [\"Jun\", \"Jul\", \"Aug\", \"Sep\", \"Oct\", 11]}"
  ));
  EXPECT_EQ("* * * 11,12 * *", parse_to_str(
    "\"month\": [\"Nov\", \"december\"]}"
  ));
}

TEST(TestStandardCrontabItem, ParseDayOfWeekNames) {
  EXPECT_EQ("* * * * 2,4,6,7 *", parse_to_str(
    "\"day_of_week\": [\"Mon\", \"WEDNESDAY\", \"frIDay\", 7]}"
  ));
  EXPECT_EQ("* * * * 1,2,3,5,6 *", parse_to_str(
    "\"day_of_week\": [\"tuesday\", \"SUN\", 2, \"thu\", 6]}"
  ));
  EXPECT_EQ("* * * * 7 *", parse_to_str("\"day_of_week\": \"SAT\"}"));
}
