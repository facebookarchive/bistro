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

#include "bistro/bistro/cron/CrontabItem.h"

#include <memory>
#include <stdexcept>

#include "folly/dynamic.h"

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
