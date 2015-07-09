/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/config/utils.h"

namespace facebook { namespace bistro {

using namespace std;
using folly::dynamic;

void update(folly::dynamic& d, const folly::dynamic& d2) {
  for (const auto& pair : d2.items()) {
    d[pair.first] = pair.second;
  }
}

folly::dynamic merge(const folly::dynamic& d, const folly::dynamic& d2) {
  folly::dynamic ret(d);
  update(ret, d2);
  return ret;
}

}}
