#pragma once

#include "folly/dynamic.h"
#include "bistro/bistro/if/gen-cpp2/scheduler_types.h"

namespace facebook { namespace bistro {

folly::dynamic toDynamic(const cpp2::BistroJobConfigFilters&);
cpp2::BistroJobConfigFilters toThrift(const folly::dynamic&);

folly::dynamic toDynamic(const cpp2::BistroJobConfig&);
cpp2::BistroJobConfig toThrift(const std::string&, const folly::dynamic&);

}}
