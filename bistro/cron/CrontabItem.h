/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <boost/date_time/local_time/local_time_types.hpp>
#include <memory>
#include <utility>

#include "folly/Optional.h"

// Add stateful Cron support for more robustness, see README for a design.

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro {

class CrontabItem {
public:
  explicit CrontabItem(boost::local_time::time_zone_ptr tz) : tz_(tz) {}
  CrontabItem(const CrontabItem&) = delete;  // can neither copy
  CrontabItem& operator=(const CrontabItem&) = delete;  // nor assign
  virtual ~CrontabItem() {}

  // Pass a null time_zone_ptr to use the local computer's timezone
  static std::unique_ptr<const CrontabItem> fromDynamic(
    const folly::dynamic&, boost::local_time::time_zone_ptr
  );

  // Affects the behavior of CrontabEvents
  virtual bool isTimezoneDependent() = 0;

  /**
   * Returns the first timestamp matching this item's selectors. Or,
   * if no match exists returns a null value.
   *
   * Note: if you are ever forced to make a CrontabItem that has to search
   * extensively, add a "max_time" to the call.  In this case, the null
   * value would mean "no match found up to max_time".
   */
  virtual folly::Optional<time_t> findFirstMatch(time_t time_since_utc_epoch)
    const = 0;

  /**
   * A compact string representation for unit tests or debugging, which sort
   * of emulates standard cron syntax.  This function is subject to change
   * without notice -- send a patch with toDynamic() if you need to inspect
   * the contents of an item in production code.  We will NOT fix your code
   * if you are using this function.  You have been warned.
   */
  virtual std::string getPrintable() const;

protected:
  boost::local_time::time_zone_ptr tz_;
};

}}
