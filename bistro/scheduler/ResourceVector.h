/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/small_vector.h>

namespace facebook { namespace bistro {

typedef folly::small_vector<
  int,
  32, /* size before overflowing to heap - not good for Bistro
       * performance to overflow */
  size_t /* size_type of the underlying vector */
> ResourceVector;

}}
