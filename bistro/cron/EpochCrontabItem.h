/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <boost/date_time/local_time/local_time_types.hpp>
#include <limits>

#include "bistro/bistro/cron/CrontabItem.h"
#include "bistro/bistro/cron/CrontabSelector.h"

// Add stateful Cron support for more robustness, see README for a design.

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro { namespace detail_cron {

class EpochCrontabItem : public CrontabItem {
public:
  EpochCrontabItem(const folly::dynamic&, boost::local_time::time_zone_ptr);
  folly::Optional<time_t> findFirstMatch(time_t time_since_utc_epoch)
    const final;
  bool isTimezoneDependent() override { return false; }

private:
  CrontabSelector epoch_sel_;
};

}}}
