/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/cron/StandardCrontabItem.h"

#include <algorithm>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/date_time/gregorian/gregorian_types.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <stdexcept>
#include <utility>

#include <folly/dynamic.h>
#include <folly/Format.h>
#include "bistro/bistro/cron/utils/date_time.h"

// Add stateful Cron support for more robustness, see README for a design.

using namespace boost::local_time;
using namespace boost::posix_time;
using namespace folly;
using namespace std;

// TODO: Make the error messages more helpful by identifying the failing
// crontab item, and its current input timestamp.

namespace facebook { namespace bistro { namespace detail_cron {

namespace {

int64_t kDaysInWeek = 7;

/**
 * boost::posix_time::to_tm sets the tm_dow field according to the standard
 * convention of 0 == Sunday, but this code starts with 1 because nobody
 * thinks of Sunday as the "0th day of the week".
 *
 * NB This isn't really portable, because in many parts of the world (e.g.
 * Russia), Monday is the first day of the week, but only boost::locale has
 * support for this, and in any case, it's safer for "1" to have a fixed
 * meaning in the Cron configs.
 */
int64_t parseMonthName(const string& s) {
  if (s == "jan" || s == "january") {
    return 1;
  } else if (s == "feb" || s == "february") {
    return 2;
  } else if (s == "mar" || s == "march") {
    return 3;
  } else if (s == "apr" || s == "april") {
    return 4;
  } else if (s == "may") {
    return 5;
  } else if (s == "jun" || s == "june") {
    return 6;
  } else if (s == "jul" || s == "july") {
    return 7;
  } else if (s == "aug" || s == "august") {
    return 8;
  } else if (s == "sep" || s == "september") {
    return 9;
  } else if (s == "oct" || s == "october") {
    return 10;
  } else if (s == "nov" || s == "november") {
    return 11;
  } else if (s == "dec" || s == "december") {
    return 12;
  }
  throw std::runtime_error("Unknown month: " + s);
}

// Same situation as for dayOfWeek, we add 1 to make the months intuitive.
int64_t parseDayOfWeek(const string& s) {
  if (s == "sun" || s == "sunday") {
    return 1;
  } else if (s == "mon" || s == "monday") {
    return 2;
  } else if (s == "tue" || s == "tuesday") {
    return 3;
  } else if (s == "wed" || s == "wednesday") {
    return 4;
  } else if (s == "thu" || s == "thursday") {
    return 5;
  } else if (s == "fri" || s == "friday") {
    return 6;
  } else if (s == "sat" || s == "saturday") {
    return 7;
  }
  throw std::runtime_error("Unknown day of week: " + s);
}

Optional<CrontabSelector> maybeMakeDayOfWeekSelector(const dynamic& d) {
  if (const auto* dow_dyn = d.get_ptr("day_of_week")) {
    return CrontabSelector(*dow_dyn, 1, kDaysInWeek, parseDayOfWeek);
  }
  return folly::none;
}

}  // detail namespace

struct MatchComputeState {
  // This search depth can be reached if e.g. the user requires Feb 29 in an
  // odd-numbered year.  We shouldn't search all the way out to year 9999
  // because this risks stack overflow (and is slow).
  const static int64_t kMaxDepth = 50;

  explicit MatchComputeState(const ptime& pt)
    : depth_(0),
      pos_(StandardCrontabItem::Position::YEAR),
      // NB hasResult_ defaults to true because there's only one way for the
      // search to fail.
      hasResult_(true) {

    auto tm = to_tm(pt);
    setVal(StandardCrontabItem::Position::YEAR, tm.tm_year + 1900);
    // January is 1
    setVal(StandardCrontabItem::Position::MONTH, tm.tm_mon + 1);
    setVal(StandardCrontabItem::Position::DAY_OF_MONTH, tm.tm_mday);
    setVal(StandardCrontabItem::Position::HOUR, tm.tm_hour);
    setVal(StandardCrontabItem::Position::MINUTE, tm.tm_min);
    if (tm.tm_sec != 0) {
      // This can happen if the DST rewind is at a time with nonzero seconds
      throw logic_error(
        format("nonzero seconds in {}", to_simple_string(pt)).str()
      );
    }
  }

  void saveResult(ptime *res) const {
    *res = ptime(
      boost::gregorian::date(
        getVal(StandardCrontabItem::Position::YEAR),
        getVal(StandardCrontabItem::Position::MONTH),
        getVal(StandardCrontabItem::Position::DAY_OF_MONTH)
      ),
      time_duration(
        getVal(StandardCrontabItem::Position::HOUR),
        getVal(StandardCrontabItem::Position::MINUTE),
        0  // seconds
      )
    );
  }

  // Sunday is 1, Monday is 2, etc
  int64_t getDayOfWeek() const {
    return boost::gregorian::date(
      getVal(StandardCrontabItem::Position::YEAR),
      getVal(StandardCrontabItem::Position::MONTH),
      getVal(StandardCrontabItem::Position::DAY_OF_MONTH)
    ).day_of_week() + 1;
  }

  int64_t getEndOfMonthDay() const {
    return boost::gregorian::gregorian_calendar::end_of_month_day(
      getVal(StandardCrontabItem::Position::YEAR),
      getVal(StandardCrontabItem::Position::MONTH)
    );
  }

  int64_t getVal(StandardCrontabItem::Position p) const {
    return vals_[static_cast<int>(p)];
  }

  void setVal(StandardCrontabItem::Position p, int64_t val) {
    int i = static_cast<int>(p);
    if (i < 0 || i >= static_cast<int>(StandardCrontabItem::Position::MAX)) {
      throw logic_error(format("Bad position {}", i).str());
    }
    vals_[i] = val;
  }

  void increaseDepth() {
    ++depth_;
    if (depth_ > kMaxDepth) {
      throw runtime_error(format(
        "No matches in {} iterations. Does your selector never match?", depth_
      ).str());
    }
  }

  int64_t depth_;
  StandardCrontabItem::Position pos_;
  int64_t vals_[static_cast<int>(StandardCrontabItem::Position::MAX)];
  bool hasResult_;
};

StandardCrontabItem::StandardCrontabItem(const dynamic& d, time_zone_ptr tz)
  : CrontabItem(tz),
    dstFixes_(0),
    selectors_{
      // Clamp at year 9999 because boost::gregorian tops out at 10000.
      // This means we fail in our code rather than in boost.
      CrontabSelector(d.getDefault("year"), 1969, 9999),
      // There is no localization.  January == 1 and names (or 3-letter
      // prefixes like "dec") must be given in English.
      CrontabSelector(d.getDefault("month"), 1, 12, parseMonthName),
      // TODO: It might be cool to add 32 with an "end_of_month" string
      // alias for "match end of month", ditto for -1 / "start of month".
      CrontabSelector(d.getDefault("day_of_month"), 1, 31),
      CrontabSelector(d.getDefault("hour"), 0, 23),
      CrontabSelector(d.getDefault("minute"), 0, 59),
    },
    maybeDayOfWeekSel_(maybeMakeDayOfWeekSelector(d)) {

  parseDstFixes(d);
  bool has_day_of_week{false}, has_day_of_month{false}, has_minute{false};
  for (const auto& pair : d.items()) {
    if (pair.first == "day_of_month") {
      has_day_of_month = true;
    } else if (pair.first == "day_of_week") {
      has_day_of_week = true;
    } else if (pair.first == "minute") {
      has_minute = true;
    } else if (
      pair.first != "year" && pair.first != "month" &&
      pair.first != "hour" && pair.first != "dst_fixes"
    ) {
      throw runtime_error(
        format("Unknown crontab item key: {}", pair.first).str()
      );
    }
  }
  if (has_day_of_month && has_day_of_week) {
    throw runtime_error("Cannot have both day_of_month and day_of_week");
  }
  if (!has_minute) {
    throw runtime_error("Must have \"minute\" selector");
  }
}

void StandardCrontabItem::parseDstFixes(const dynamic& d) {
  auto dst_fix_list = d.find("dst_fixes");
  if (dst_fix_list == d.items().end() || !dst_fix_list->second.isArray()) {
    throw runtime_error("Specify DST handling in the \"dst_fixes\" list");
  }
  int dst_policies = 0;  // Ensures we hit each policy exactly once.
  auto add_policy = [&dst_policies](int policy){
    if (dst_policies & policy) {
      throw runtime_error(
        format("DST policy {} already had a fix", policy).str()
      );
    }
    dst_policies |= policy;
  };
  for (const auto &fix_name : dst_fix_list->second) {
    if (fix_name == "skip") {
      // Don't touch dstFixes_: the absence of the "unskip" flag means "skip"
      add_policy(kDSTPolicyForward);
    } else if (fix_name == "unskip") {
      dstFixes_ |= kDSTForwardDoUnskip;
      add_policy(kDSTPolicyForward);
    } else if (fix_name == "repeat_use_only_early") {
      dstFixes_ |= kDSTRewindUseEarly;
      add_policy(kDSTPolicyRewind);
    } else if (fix_name == "repeat_use_only_late") {
      dstFixes_ |= kDSTRewindUseLate;
      add_policy(kDSTPolicyRewind);
    } else if (fix_name == "repeat_use_both") {
      dstFixes_ |= kDSTRewindUseEarly | kDSTRewindUseLate;
      add_policy(kDSTPolicyRewind);
    } else {
      throw runtime_error(
        format("Unknown dst_fixes entry: {}", fix_name).str()
      );
    }
  }
  auto missing = kRequiredDSTPolicies & ~dst_policies;
  if (missing) {
    throw runtime_error(format(
      "\"dst_fixes\" needs {} DST policy rules", missing
    ).str());
  }
}

void StandardCrontabItem::findMatchingDayOfWeek(
    MatchComputeState *s,
    int64_t carry_steps) const {

  auto dow = s->getDayOfWeek();
  if (dow < 1 || dow > kDaysInWeek || carry_steps > 1) {  // paranoia
    throw logic_error(format("bad weekday {} {}", dow, carry_steps).str());
  }

  int64_t new_dow;
  bool carry_next;
  tie(new_dow, carry_next) =
    maybeDayOfWeekSel_->findFirstMatch(dow + carry_steps);

  int64_t steps = 0;
  if (carry_next) {
    if (new_dow > dow) {
      throw logic_error(format("dow {} carried to {}", dow, new_dow).str());
    }
    steps += new_dow + kDaysInWeek - dow;
  } else {
    if (new_dow < dow) {
      throw logic_error(format(
        "dow {} did not carry to {}", dow, new_dow
      ).str());
    }
    steps += new_dow - dow;
  }
  findMatchingPositionsImpl(s, steps);
}

void StandardCrontabItem::findMatchingPositions(
    MatchComputeState *s,
    int64_t carry_steps) const {

  if (s->pos_ == Position::DAY_OF_MONTH && maybeDayOfWeekSel_) {
    // This will redirect to findMatchingPositionsImpl() after a short detour.
    return findMatchingDayOfWeek(s, carry_steps);
  }
  if (carry_steps > 1) {
    throw logic_error(format("bad carry_steps {}", carry_steps).str());
  }
  findMatchingPositionsImpl(s, carry_steps);
}

namespace {

StandardCrontabItem::Position prevPos(StandardCrontabItem::Position p) {
  return static_cast<StandardCrontabItem::Position>(static_cast<int>(p) - 1);
}

StandardCrontabItem::Position nextPos(StandardCrontabItem::Position p) {
  return static_cast<StandardCrontabItem::Position>(static_cast<int>(p) + 1);
}

}  // detail namespace

void StandardCrontabItem::findMatchingPositionsImpl(
  MatchComputeState *s, int64_t carry_steps
) const {
  s->increaseDepth();
  const auto& sel = selectors_[(int)s->pos_];
  int64_t val = s->getVal(s->pos_);
  if (val < sel.getMin() || val > sel.getMax()) {
    throw logic_error(format(
      "{} not in [{}, {}]", val, sel.getMin(), sel.getMax()
    ).str());
  }
  int64_t new_val;
  bool carry_next;
  tie(new_val, carry_next) = sel.findFirstMatch(val + carry_steps);
  if (
    !carry_next && s->pos_ == Position::DAY_OF_MONTH &&
    new_val > s->getEndOfMonthDay()
  ) {
    carry_next = true;  // findFirstMatch may return 30 in February
  }
  if (carry_next) {
    if (s->pos_ == Position::YEAR) {
      // We exceeded the final year in matched by this crontab item,
      // so we won't be able to find a match.
      s->hasResult_ = false;
      return;
    }
    s->pos_ = prevPos(s->pos_);
    return findMatchingPositions(s, true);
  }
  // We know we didn't wrap around (carry_next is false here), so the value
  // can't go down (and goes up if we had an input carry).
  if (new_val < val || (carry_steps && new_val == val)) {
    throw logic_error(
      format("{} + {} went to {}", val, carry_steps, new_val).str()
    );
  }
  s->setVal(s->pos_, new_val);
  s->pos_ = nextPos(s->pos_);
  if (s->pos_ == Position::MAX) {
    return;  // All done!
  }
  // If this position advanced, we should reset all later ones, like 199 => 200
  if (new_val != val) {
    for (auto p = s->pos_; p < Position::MAX; p = nextPos(p)) {
      s->setVal(p, selectors_[(int)p].getMin());
    }
  }
  return findMatchingPositions(s, false);
}

namespace {

// This random-access iterator lets us use upper_bound() to search time.
class PTimeSecondsIterator
  : public boost::iterator_facade<
      PTimeSecondsIterator,
      const ptime,
      boost::random_access_traversal_tag> {

public:
  explicit PTimeSecondsIterator(const ptime& pt) : pt_(pt) {}

private:
  friend class boost::iterator_core_access;

  bool equal(const PTimeSecondsIterator& other) const {
    return pt_ == other.pt_;
  }

  const ptime& dereference() const {
    return pt_;
  }

  void increment() {
    pt_ += seconds(1);
  }

  void advance(int n) {
    pt_ += seconds(n);
  }

  int distance_to(const PTimeSecondsIterator& other) const {
    return (other.pt_ - pt_).total_seconds();
  }

  ptime pt_;
};

// Find the second immediately preceding the DST skip.  We assume but don't
// check that not_a_time maps to an isNotATime() value in this timezone.
time_t findPreSkipUTCTime(const ptime& not_a_time, time_zone_ptr tz) {
  // Walk back up to 24 hours to find a valid time label to lower-bound our
  // binary search. This is wrong in pathological cases where DST starts
  // and ends less than 1 hour apart, but...  whatever.
  ptime first_pt;
  for (int h : {1, 2, 3, 6, 12, 24, 25}) {
    first_pt = not_a_time - hours(h);
    if (!timezoneLocalPTimeToUTCTimestamps(first_pt, tz).isNotATime()) {
      break;
    }
    // TODO: An optimization that I'm too lazy to test now would be:
    // not_a_time = first_pt;
    if (h > 24) {  // inside the loop due to laziness
      throw logic_error(format(
        "No valid time found 24h before {}", to_simple_string(not_a_time)
      ).str());
    }
  }

  auto first = PTimeSecondsIterator(first_pt);
  auto last = PTimeSecondsIterator(not_a_time);
  auto first_non_time =
      *upper_bound(first, last, 0, [tz](int /*_0*/, ptime pt) {
        return timezoneLocalPTimeToUTCTimestamps(pt, tz).isNotATime();
      });
  // The search cannot fail and we cannot even check since last is the only
  // known isNotATime() value. Iterating in reverse would work, but no need.
  return timezoneLocalPTimeToUTCTimestamps(
    first_non_time - seconds(1), tz
  ).getUnique();
}

// Assuming t is one of ts.dst_time, ts.non_dst_time, return true iff it's
// the earlier one.
bool isBeforeRewind(time_t t, const UTCTimestampsForLocalTime& ts) {
  if (t != ts.dst_time && t != ts.non_dst_time) {
    throw logic_error(format(
      "{} not in {}, {}", t, ts.dst_time, ts.non_dst_time
    ).str());
  }
  if (!ts.isAmbiguous()) {
    throw logic_error(
      format("{} not ambiguous in local tz", t).str()
    );
  }
  return t == min(ts.dst_time, ts.non_dst_time);
}

bool isTimeBeforeRewind(time_t t, time_zone_ptr tz) {
  return isBeforeRewind(t, timezoneLocalPTimeToUTCTimestamps(
    utcPTimeToTimezoneLocalPTime(from_time_t(t), tz), tz
  ));
}

// Find the first point after the DST clock rewind, given two variants of an
// ambiguous local time.
time_t findRewindTime(time_t a, time_t b, time_zone_ptr tz) {
  // This sanity check is a little redundant.
  if (!isTimeBeforeRewind(a, tz) || isTimeBeforeRewind(b, tz)) {
    throw logic_error(
      format("bad ambiguous pair: {} {}", a, b).str()
    );
  }
  auto first = boost::counting_iterator<time_t>(a);
  auto last = boost::counting_iterator<time_t>(b + 1);
  auto rewind_t = *std::partition_point(first, last, [tz](time_t test_t) {
    return isTimeBeforeRewind(test_t, tz);
  });
  if (rewind_t > b) {
    throw logic_error(format("No DST rewind between {} and {}", a, b).str());
  }
  return rewind_t;
}

}  // detail namespace

Optional<UTCTimestampsForLocalTime> StandardCrontabItem::
    findUTCTimestampsForFirstMatch(time_t t, ptime *match_pt) const {

  // Compute the "year-...-hour-minute" in the given timezone.
  auto local_pt = utcPTimeToTimezoneLocalPTime(from_time_t(t), tz_);

  // Search for a matching local time label
  MatchComputeState state(local_pt);
  findMatchingPositions(&state, false);
  if (!state.hasResult_) {
    return none;
  }
  state.saveResult(match_pt);

  auto utc_ts = timezoneLocalPTimeToUTCTimestamps(*match_pt, tz_);

  // Might we need to re-do the search due to the DST "rewind"?
  //
  // Look below at the diagram for the isAmbiguous() case of
  // findFirstMatch().  Suppose our policy is "late" or "both", and t is 38 --
  // right after match5.  We want our next match to be match6, but if we
  // just search forward from local_pt we'll end up leaving the ambiguous
  // zone and finding match8.  To prevent this, we need to:
  //   1) determine if local_pt is in the ambiguous zone, before the rewind
  //   2) if our match exits the ambiguous zone, re-search from its beginning
  auto ts = timezoneLocalPTimeToUTCTimestamps(local_pt, tz_);
  if (ts.isAmbiguous() && isBeforeRewind(t, ts)) {
    auto ordered = ts.getBothInOrder();
    auto rewind_t = findRewindTime(ordered.first, ordered.second, tz_);
    auto first_ambiguous = ordered.first - (ordered.second - rewind_t);
    auto first_unambiguous = ordered.second + (rewind_t - ordered.first);
    // Rewind the search if the match is outside of our initial ambiguous zone
    if (
      t != first_ambiguous &&  // Prevent infinite recursion
      max(utc_ts.dst_time, utc_ts.non_dst_time) >= first_unambiguous
    ) {
      // This may not find anything either, but that just means that there
      // are no matches in the DST-ambiguous time period, which is okay.
      return findUTCTimestampsForFirstMatch(first_ambiguous, match_pt);
    }
  }
  return utc_ts;  // Search did not wrap around
}

Optional<time_t> StandardCrontabItem::findFirstMatch(
    time_t time_since_utc_epoch) const {

  auto maybe_time = findFirstMatchImpl(time_since_utc_epoch);
  if (maybe_time.has_value() && maybe_time.value() < time_since_utc_epoch) {
    throw logic_error(format(
      "findFirstMatch went back in time {} -> {}",
      time_since_utc_epoch, maybe_time.value()
    ).str());
  }
  return maybe_time;
}

Optional<time_t> StandardCrontabItem::findFirstMatchImpl(
    time_t time_since_utc_epoch) const {

  // We discard seconds in our search, so if the input is 12:43:45, and our
  // selectors match all minutes, we would return 12:43:00.  In particular,
  // this can get us stuck -- to attempt to iterate, we'll go from 43:01 to
  // 43:00 again, and so on forever.  The fix is to round up the input to
  // the next minute.
  //
  // TODO: Morally, it's more correct to make this adjustment in the
  // constructor of MatchComputeState, since that would work even if DST
  // adjustments were made at times with non-zero seconds, but this is just
  // so much easier:
  time_since_utc_epoch += (60 - time_since_utc_epoch % 60) % 60;

  ptime match_pt;
  auto maybe_utc_ts =
    findUTCTimestampsForFirstMatch(time_since_utc_epoch, &match_pt);
  if (!maybe_utc_ts.has_value()) {
    return none;
  }
  auto utc_ts = maybe_utc_ts.value();

  // If the local time label is not valid due to a DST skip, we need a
  // stateless choice for whether, and how, to map such events to the input:
  //
  // Input time:        27 xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx 28
  //                     \                                    /
  // DST skip:   --------->begin skip****************end skip---------------->
  //              19            ??          ??                       36
  // Matches:     (match3)      match4      match5                   (match6)
  // Output functions for our DST-skip policies:
  // Skip:       336666666666666666666666666666666666666666666666666667777777
  // Unskip:     3344444444xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx6666666667777777
  //
  // For unskip, we essentially map all the skipped events to the time point
  // immediately preceding the clock forward.  It's okay if that time point
  // already had a real matching event (in this case "unskip" and "skip"
  // behave identically).
  //
  // Caveat: Above, match5 is always skipped. This is good -- if several
  // events for this crontab item landed inside a 0-length "interval" it
  // makes complete sense to "unskip" only one.
  if (utc_ts.isNotATime()) {
    // This *has* to be the last second before the skip so that we're sure
    // our output >= input.  This violates our general preference to align
    // on the minute, but this is the only sane way.
    auto pre_skip_utc_t = findPreSkipUTCTime(match_pt, tz_);
    if (time_since_utc_epoch > pre_skip_utc_t) {
      throw logic_error(format(
        "Input was after skip: {} > {}", time_since_utc_epoch, pre_skip_utc_t
      ).str());
    }
    if (dstFixes_ & kDSTForwardDoUnskip) {
      return pre_skip_utc_t;
    }
    // TODO: With some additional selectors, it would be possible to make a
    // crontab item that *only* matches skipped local times, and this would
    // cause an infinite recursion here...
    return findFirstMatch(pre_skip_utc_t + 1);
  // If the local time is ambiguous due to a DST rewind, we need a stateless
  // heuristic to map an input time to one or more of the utc_ts timestamps:
  //
  // Input time:     22                   43                   64
  // DST shift : --->begin ambiguity----->rewind clock-------->end ambiguity->
  //                   24         35        45         56           69
  // Matches:    ----->match4---->match5----match6---->match7-------(match8)->
  // Output functions for our DST ambiguity resolution policies:
  // Early:      444444455555555555888888888888888888888888888888888899999999
  // Late:       666666666666666666666666666677777777777888888888888899999999
  // Both:       444444455555555555666666666677777777777888888888888899999999
  } else if (utc_ts.isAmbiguous()) {
    // TODO: an optimization I'm too lazy to implement for the "early" &
    // "late" heuristics: compute the rewind time, and the first unambiguous
    // time that follows (use some sort of lazy memoized way to share it
    // with findUTCTimestampsForFirstMatch()).  Use these to skip over the
    // ambiguous period that's irrelevant to the current heuristic.
    auto ordered_ts = utc_ts.getBothInOrder();
    if (time_since_utc_epoch <= ordered_ts.first) {
      if (!(dstFixes_ & kDSTRewindUseEarly)) {  // Skip the early repeat
        return findFirstMatch(ordered_ts.first + 1);  // See optimization above
      }
      return ordered_ts.first;
    }
    if (!(dstFixes_ & kDSTRewindUseLate)) {  // Skip the late repeat
      return findFirstMatch(ordered_ts.second + 1);  // See optimization above
    }
    return ordered_ts.second;
  }
  return utc_ts.getUnique();
}

string StandardCrontabItem::getPrintable() const {
  string s;
  folly::toAppend(selectors_[4].getPrintable(), ' ', &s);
  folly::toAppend(selectors_[3].getPrintable(), ' ', &s);
  folly::toAppend(selectors_[2].getPrintable(), ' ', &s);
  folly::toAppend(selectors_[1].getPrintable(), ' ', &s);
  if (maybeDayOfWeekSel_.has_value()) {
    folly::toAppend(maybeDayOfWeekSel_->getPrintable(), ' ', &s);
  } else {
    folly::toAppend("* ", &s);
  }
  folly::toAppend(selectors_[0].getPrintable(), &s);
  return s;
}

}}}
