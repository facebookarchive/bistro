/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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

ShuffledRange<std::vector<NodePtr>::const_iterator> Nodes::shuffled() const {
  return ShuffledRange<std::vector<NodePtr>::const_iterator>(
    nodes_.begin(),
    nodes_.end()
  );
}

Nodes::LevelIterRange Nodes::iterateOverLevel(NodeLevel level) const {
  auto tester = detail::DoesNodeBelongToLevel(level);
  return Nodes::LevelIterRange(
    Nodes::LevelIter(tester, nodes_.begin(), nodes_.end()),
    Nodes::LevelIter(tester, nodes_.end(), nodes_.end())
  );
}

NodePtr Nodes::getNodeVerySlow(const std::string& name) const {
  for (const auto& n : nodes_) {
    if (n->name() == name) {
      return n;
    }
  }
  return nullptr;
}

}}
