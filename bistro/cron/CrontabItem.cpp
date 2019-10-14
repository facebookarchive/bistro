/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/cron/CrontabItem.h"

#include <memory>
#include <stdexcept>

#include <folly/dynamic.h>

#include "bistro/bistro/cron/EpochCrontabItem.h"
#include "bistro/bistro/cron/StandardCrontabItem.h"

// Add stateful Cron support for more robustness, see README for a design.

using namespace folly;
using namespace std;

namespace facebook { namespace bistro {

unique_ptr<const CrontabItem> CrontabItem::fromDynamic(
    const dynamic& d, boost::local_time::time_zone_ptr tz) {

  if (!d.isObject()) {
    throw runtime_error("CrontabItem must be an object");
  }
  if (d.find("epoch") != d.items().end()) {
    return unique_ptr<CrontabItem>(new detail_cron::EpochCrontabItem(d, tz));
  }
  return unique_ptr<CrontabItem>(new detail_cron::StandardCrontabItem(d, tz));
}

string CrontabItem::getPrintable() const {
  throw logic_error("getPrintable not implemented");
}

}}
