/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/nodes/Nodes.h"

namespace facebook { namespace bistro {

boost::iterator_range<std::vector<std::shared_ptr<const Node>>::const_iterator>
iterate_non_instance_nodes(const Nodes& n) {
  CHECK(n.size() > 0);
  return boost::make_iterator_range(n.begin() + 1, n.end());
}

std::shared_ptr<const Node> getNodeVerySlow(
    const Nodes& nodes,
    const std::string& name) {
  for (const auto& node : nodes) {
    if (node->name() == name) {
      return node;
    }
  }
  return nullptr;
}

}}
