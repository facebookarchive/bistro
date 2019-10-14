/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/cron/CrontabSelector.h"

#include <algorithm>
#include <stdexcept>

#include <folly/Conv.h>
#include <folly/dynamic.h>
#include <folly/Format.h>
#include <folly/json.h>

// Add stateful Cron support for more robustness, see README for a design.

using namespace folly;
using namespace std;

namespace facebook { namespace bistro { namespace detail_cron {

CrontabSelector::CrontabSelector(
  const dynamic &d,
  int64_t min_val,
  int64_t max_val,
  function<int64_t(const string& lc_str)> str_to_value
) : start_(min_val),
    end_(max_val),
    period_(1),
    minVal_(min_val),
    maxVal_(max_val) {

  switch (d.type()) {
    case dynamic::Type::INT64:
    case dynamic::Type::STRING:
      sortedValues_.emplace_back(parseValue(d, str_to_value));
      break;
    case dynamic::Type::ARRAY:
      for (const auto& val : d) {
        sortedValues_.emplace_back(parseValue(val, str_to_value));
      }
      // If somebody specifies [] for a selector, we have to silently
      // accept it, since PHP's JSON library can morph {} into [], and {}
      // must mean "default selector accepting all values."
      break;
    case dynamic::Type::OBJECT:
      for (const auto& pair : d.items()) {
        // Interval is first so that it doesn't accept strings like "jan"
        if (pair.first == "period") {
          period_ = pair.second.asInt();
          if (period_ < 1 || period_ >= maxVal_ - minVal_) {
            throw runtime_error(format(
              "period not in [1, {}]: {}", maxVal_ - minVal_, period_
            ).str());
          }
          continue;
        }
        // For start & end, we are happy to accept string names
        auto val = parseValue(pair.second, str_to_value);
        if (pair.first == "start") {
          start_ = val;
        } else if (pair.first == "end") {
          end_ = val;
        } else {
          throw runtime_error(format("Unknown key: {}", pair.first).str());
        }
      }
      // If we got an empty object, no problem -- this selector will
      // follow the default of "match everything".
      break;
    default:
      throw runtime_error(format(
        "Bad type for crontab selector: {}", d.typeName()
      ).str());
  }
  sort(sortedValues_.begin(), sortedValues_.end());
}

pair<int64_t, bool> CrontabSelector::findFirstMatch(int64_t t) const {
  if (!sortedValues_.empty()) {
    // If this were stateful, we could remember where the previous item was,
    // but as is, we incur the log(n) search time every time.
    auto i = lower_bound(sortedValues_.begin(), sortedValues_.end(), t);
    if (i == sortedValues_.end()) {  // Wrap to the start
      return make_pair(*sortedValues_.begin(), true);
    }
    return make_pair(*i, false);
  }

  if (t < start_) {
    return make_pair(start_, false);
  }
  int64_t next = t + (period_ - (t - start_) % period_) % period_;
  if (next > end_) {
    return make_pair(start_, true);
  }
  return make_pair(next, false);
}

string CrontabSelector::getPrintable() const {
  string s;
  for (const auto &v : sortedValues_) {
    folly::toAppend(v, ',', &s);
  }
  if (start_ != minVal_ || end_ != maxVal_ || period_ != 1) {
    if (!sortedValues_.empty()) {
      s[s.size() - 1] = '/';
    }
    folly::toAppend(start_, '-', end_, ':', period_, &s);
  } else if (sortedValues_.empty()) {
    folly::toAppend('*', &s);
  } else {
    s.pop_back();
  }
  return s;
}

int64_t CrontabSelector::parseValue(
    const dynamic& d,
    function<int64_t(const string& lc_str)> str_to_value) {

  int64_t res;
  if (d.isInt()) {
    res = d.asInt();
  } else if (d.isString()) {
    auto s = d.asString();
    if (str_to_value == nullptr) {
      throw runtime_error("Cannot parse string " + s);
    }
    transform(s.begin(), s.end(), s.begin(), ::tolower);
    res = str_to_value(s);
  } else {
    throw runtime_error(format("Cannot parse {}", folly::toJson(d)).str());
  }
  if (res < minVal_ || res > maxVal_) {
    throw runtime_error(format(
      "Value {} out of range [{}, {}]", res, minVal_, maxVal_
    ).str());
  }
  return res;
}

}}}
