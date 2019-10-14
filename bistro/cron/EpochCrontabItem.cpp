/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/cron/EpochCrontabItem.h"

#include <stdexcept>

#include <folly/dynamic.h>
#include <folly/json.h>

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
      "Can only have the 'epoch' key: " + folly::toJson(d)
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
