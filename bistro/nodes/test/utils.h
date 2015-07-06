#pragma once

#include "bistro/bistro/nodes/Nodes.h"

namespace facebook { namespace bistro {

boost::iterator_range<std::vector<NodePtr>::const_iterator>
iterate_non_instance_nodes(const Nodes& n) {
  CHECK(n.size() > 0);
  return boost::make_iterator_range(n.begin() + 1, n.end());
}

}}
