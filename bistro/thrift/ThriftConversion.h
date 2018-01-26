/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/dynamic.h>
#include "bistro/bistro/if/gen-cpp2/scheduler_types.h"

namespace facebook { namespace bistro {

folly::dynamic toDynamic(const cpp2::BistroJobConfigFilters&);
cpp2::BistroJobConfigFilters toThrift(const folly::dynamic&);

folly::dynamic toDynamic(const cpp2::BistroJobConfig&);
cpp2::BistroJobConfig toThrift(const std::string&, const folly::dynamic&);

}}
