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

#include "bistro/bistro/cron/EpochCrontabItem.h"

#include <stdexcept>

#include "folly/dynamic.h"
#include "folly/json.h"

// Add stateful Cron support for more robustness, see README for a design.

using namespace folly;
using namespace std;

namespace facebook { namespace bistro { namespace detail_cron {

EpochCrontabItem::EpochCrontabItem(
  const dynamic& d, boost::local_time::time_zone_ptr tz
) : CrontabItem(tz),
    epoch_sel_(d.getDefault("epoch"), 0, std::numeric_limits<time_t>::max()) {

  if (d.size() != 1) {
    throw runtime_error(
      "Can only have the 'epoch' key: " + folly::toJson(d).toStdString()
    );
  }
}

Optional<time_t> EpochCrontabItem::findFirstMatch(
    time_t time_since_utc_epoch) const {

  auto time_and_carry =  epoch_sel_.findFirstMatch(time_since_utc_epoch);
  Optional<time_t> res;
  // It's only a match if the selector didn't wrap around to the beginning
  if (!time_and_carry.second) {
    res = time_and_carry.first;
  }
  return res;
}

}}}
