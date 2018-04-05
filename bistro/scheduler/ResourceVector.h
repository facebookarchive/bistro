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

#include <folly/small_vector.h>

namespace facebook { namespace bistro {

typedef folly::small_vector<
  int,
  32, /* size before overflowing to heap - not good for Bistro
       * performance to overflow */
  size_t /* size_type of the underlying vector */
> ResourceVector;

}}
