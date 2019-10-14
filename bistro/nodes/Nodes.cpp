/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/utils/hostname.h"

DEFINE_string(
  instance_node_name, "",
  "Name to use for the instance node, or local host if empty. This is useful "
  "if you have instance node resources with a central scheduler, whose host "
  "name can change during fail-overs."
);

namespace facebook { namespace bistro {

using namespace std;

Nodes::Nodes() {
  nodes_.emplace_back(make_shared<Node>(
    getInstanceNodeName(),
    0,  // level
    true  // enabled
  ));
}

std::string Nodes::getInstanceNodeName() {
  std::string name = FLAGS_instance_node_name.empty()
    ? getLocalHostName() : FLAGS_instance_node_name;
  return name;
}

Nodes::LevelIterRange Nodes::iterateOverLevel(NodeLevel level) const {
  auto tester = detail::DoesNodeBelongToLevel(level);
  return Nodes::LevelIterRange(
    Nodes::LevelIter(tester, nodes_.begin(), nodes_.end()),
    Nodes::LevelIter(tester, nodes_.end(), nodes_.end())
  );
}

}}
