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

#include <vector>
#include <utility>

#include "bistro/bistro/config/Node.h"

namespace facebook { namespace bistro {

class Config;
class NodeConfig;
class Nodes;

// A utility for node fetchers that generate nodes based on their parents.
std::pair<NodeLevel, std::vector<NodePtr>> getMyLevelAndParents(
    const Config& config,
    const NodeConfig& node_config,
    const Nodes* all_nodes);

}}
