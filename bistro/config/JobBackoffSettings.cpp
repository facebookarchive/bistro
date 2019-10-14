/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/JobBackoffSettings.h"

#include "bistro/bistro/utils/Exception.h"
#include <folly/json.h>

namespace facebook { namespace bistro {

using namespace std;
using namespace folly;

JobBackoffSettings::JobBackoffSettings()
  : JobBackoffSettings(dynamic::array(
      15, 30, 60, 300, 900, 3600, 7200, 21600, 86400, "repeat"
    )) {}

JobBackoffSettings::JobBackoffSettings(const dynamic& d) : repeat_(false) {
  if (!d.isArray()) {
    throw BistroException("Expected array; got ", folly::toJson(d));
  }
  if (d.empty()) {
    throw BistroException("Backoff setting is empty");
  }
  for (const auto& item : d) {
    if (item.isInt()) {
      int val = item.asInt();
      if (val <= 0) {
        throw BistroException("Backoff times must be positive: ", val);
      }
      if (!values_.empty()) {
        if (values_.back() == val) {
          throw BistroException("Duplicate backoff time: ", val);
        }
        if (values_.back() > val) {
          throw BistroException("Backoff times must be in increasing order");
        }
      }
      values_.push_back(val);
    } else if (item.isString()) {
      const auto& s = item.asString();
      if (s == "repeat") {
        if (values_.empty()) {
          throw BistroException("No backoff interval given before 'repeat'");
        }
        repeat_ = true;
      } else if  (s != "fail") {
        throw BistroException("Unknown string in backoff settings: ", s);
      }
      break;
    } else {
      throw BistroException("Invalid backoff value: ", folly::toJson(item));
    }
  }
}

cpp2::BackoffDuration JobBackoffSettings::getNext(
    const cpp2::BackoffDuration& cur_backoff) const {
  CHECK(!cur_backoff.noMoreBackoffs);
  auto it = upper_bound(values_.begin(), values_.end(), cur_backoff.seconds);
  cpp2::BackoffDuration new_backoff;
  if (it != values_.end()) {
    new_backoff.noMoreBackoffs = false;
    new_backoff.seconds = *it;
  } else if (repeat_) {
    new_backoff.noMoreBackoffs = false;
    new_backoff.seconds = values_.back();
  } else {
    new_backoff.noMoreBackoffs = true;
    // This is only used when a job sets "backoff": ["fail"], and then
    // issues an "incomplete_backoff" status.  One minute seems
    // inoffensively short, but long enough that other tasks would generally
    // start running before this one retries.
    new_backoff.seconds = 60;
  }
  return new_backoff;
}

dynamic JobBackoffSettings::toDynamic() const {
  dynamic d(values_.begin(), values_.end());
  if (repeat_) {
    d.push_back("repeat");
  } else {
    d.push_back("fail");  // ["fail"] should produce ["fail"], not ""
  }
  return d;
}

bool JobBackoffSettings::operator==(const JobBackoffSettings& other) const {
  return repeat_ == other.repeat_ && values_ == other.values_;
}

bool JobBackoffSettings::operator!=(const JobBackoffSettings& other) const {
  return !(*this == other);
}

}}
