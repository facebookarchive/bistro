/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
std::pair<NodeLevel, std::vector<std::shared_ptr<const Node>>>
getMyLevelAndParents(
    const Config& config,
    const NodeConfig& node_config,
    const Nodes* all_nodes);

}}
