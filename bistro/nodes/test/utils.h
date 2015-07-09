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

#include "bistro/bistro/nodes/Nodes.h"

namespace facebook { namespace bistro {

boost::iterator_range<std::vector<NodePtr>::const_iterator>
iterate_non_instance_nodes(const Nodes& n) {
  CHECK(n.size() > 0);
  return boost::make_iterator_range(n.begin() + 1, n.end());
}

}}
