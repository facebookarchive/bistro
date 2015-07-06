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
