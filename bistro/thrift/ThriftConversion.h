/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
