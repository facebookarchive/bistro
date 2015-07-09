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

#include <folly/dynamic.h>

namespace facebook { namespace bistro {

void update(folly::dynamic&, const folly::dynamic&);

folly::dynamic merge(const folly::dynamic&, const folly::dynamic&);

}}
