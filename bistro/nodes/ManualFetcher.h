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

#include "bistro/bistro/nodes/NodeFetcher.h"

namespace facebook { namespace bistro {

class Nodes;

/**
 * NodeFetcher that is manually configured. The configuration format is:
 *
 * "node_source_prefs" : {
 *   "node1" : ["child1", "child2", "child3"],
 *   "node2" : ["child4", "child5", "child6"],
 *   "node3" : "node2"
 * }
 *
 * It's a map of node to list of children for that node. Any nodes not
 * listed in the children list of another node will default to being
 * children of the instance level.
 *
 * Note that if you specify the same child name twice, you are going to end
 * up with two different nodes, or replicas, rather than with two different
 * parents.
 */
class ManualFetcher : public NodeFetcher {

public:
 void fetch(const Config&, const NodeConfig&, Nodes*) const override;
};

}}
