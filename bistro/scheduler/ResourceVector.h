#pragma once

#include "folly/small_vector.h"

namespace facebook { namespace bistro {

typedef folly::small_vector<
  int,
  32, /* size before overflowing to heap - not good for Bistro
       * performance to overflow */
  uint8_t /* size_type of the underlying vector */
> ResourceVector;

}}
