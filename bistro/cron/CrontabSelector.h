/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <vector>
#include <string>

// Add stateful Cron support for more robustness, see README for a design.

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro { namespace detail_cron {

/**
 * A CrontabSelector is a wildcard for matching some periodic integer value
 * (e.g.  days of week, years, minutes, ...).
 *
 * CrontabSelector is a union type, representing either:
 *   - A list of one or more values to match. E.g. January & March.
 *   - A start, an end, and a period. This range type does not wrap:
 *     starting from Tuesday with a period of 6 is equivalent to only
 *     Tuesday.
 *
 * A selector knows its minimum and maximum permissible value. For example,
 * a day-of-month is between 1 and 31, but real months may have fewer days,
 * which is handled at a higher level -- the selector is meant to apply
 * uniformly to all months.
 *
 * Some selectors also support case-insensitive string aliases for their
 * integer values (either the 3-letter prefix or the full name is okay).
 * For example, "sun" and "Sunday" would map to 1, while "sAt" is 7.  In the
 * month type, "july" maps to 7, etc.  Note that these strings are not
 * localized -- specs are English-only, but this is a feature, since
 * otherwise a Cron configuration might be ambiguous depending on the system
 * locale.  Also note that the day-of-week numbering is similarly
 * unlocalized English, so the Russians who think Sunday == 7 will have to
 * cope.
 *
 * Selectors are specified using JSON. Here are some month selectors:
 *
 *   5                              // The value 5, or May
 *   "Apr"                          // The value 4, or April
 *   ["Jun", "Jul", 12]             // June, July, or December
 *   {"period": 1}                  // Match any of the 12 months
 *   {"start": "Aug", "end": 11}    // August through November, inclusive
 *   {"period": 2, "end": "Jul"}    // January, March, May, or July
 *
 * We do not implement the traditional Cron syntax because it's hard to read
 * and verify, and implies the presence of some Cron misfeatures that we do
 * not support.  A reasonable patch adding such parser would be accepted,
 * however.
 */
class CrontabSelector {
public:

  CrontabSelector(CrontabSelector&&) = default;
  CrontabSelector& operator=(CrontabSelector&&) = default;  // can move, but
  CrontabSelector(const CrontabSelector&) = delete;  // can neither copy
  CrontabSelector& operator=(const CrontabSelector&) = delete;  // nor assign
  virtual ~CrontabSelector() {}

  /**
   * Make the selector from a JSON object (see the class docblock).
   */
  CrontabSelector(
    const folly::dynamic &d,
    int64_t min_val,
    int64_t max_val,
    // Use this callback to parse lowercase string values (e.g. thursday => 5)
    std::function<int64_t(const std::string& lc_str)> str_to_value = nullptr
  );

  /**
   * Returns the first t_match >= t such that t_match matches this selector.
   * If no match is found, wraps around to the selector's first element, and
   * sets the "carry" bool to true.  Otherwise, that value is false.
   *
   * Note: no modular arithmetic happens here -- as soon as we exceed end_,
   * we simply reset to start_.  This is not the full story for
   * day-of-month, so StandardCrontabItem has to do some extra work there.
   * The simple "wrap to start" behavior is correct because with modular
   * arithmetic a "week" selector starting on Tuesday with a stride of 6
   * would accept any day of the week, which is far more surprising than
   * only accepting Tuesday.
   */
  std::pair<int64_t, bool> findFirstMatch(int64_t t) const;

  /**
   * A compact string representation for unit tests or debugging, which sort
   * of emulates standard cron syntax.  This function is subject to change
   * without notice -- send a patch with toDynamic() if you need to inspect
   * the contents of a selector in production code.  We will NOT fix your
   * code if you are using this function.  You have been warned.
   */
  std::string getPrintable() const;

  int64_t getMin() const { return minVal_; }
  int64_t getMax() const { return maxVal_; }

  // An empty selector matches all allowable values / is a no-op
  bool empty() const {
    return
      sortedValues_.empty() &&
      minVal_ == start_ && maxVal_ == end_ && 1 == period_;
  }

private:
  int64_t parseValue(
    const folly::dynamic& d,
    std::function<int64_t(const std::string& lc_str)> str_to_value
  );

  std::vector<int64_t> sortedValues_;
  // These three are unused when sortedValues_ is set
  int64_t start_;
  int64_t end_;
  int64_t period_;

  // Default values for start & end, also used for range-checking.
  const int64_t minVal_;
  const int64_t maxVal_;
};

}}}
