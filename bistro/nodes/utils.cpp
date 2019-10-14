/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/utils.h"

#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/nodes/Nodes.h"

namespace facebook { namespace bistro {

using namespace std;

// Copy the parents because otherwise the node fetcher would be iterating
// and appending at the same time, which is probably not safe, or at least
// inefficient (iterating over newly added nodes).  This is better than
// copying the new nodes, because typically we'll have more children than
// parents.
pair<NodeLevel, vector<std::shared_ptr<const Node>>> getMyLevelAndParents(
    const Config& config,
    const NodeConfig& node_config,
    const Nodes* all_nodes) {
  const int parent_level = config.levels.lookup(
    node_config.prefs.requireConvert<string>("parent_level")
  );
  if (parent_level == config.levels.NotFound) {
    throw BistroException(parent_level, " is not a known level");
  }
  const int my_level = parent_level + 1;
  if (config.getNumConfiguredLevels() < my_level) {
    throw BistroException("You must define at least ", my_level, " levels");
  }

  auto p = all_nodes->iterateOverLevel(parent_level);
  return std::make_pair(
    my_level,
    vector<std::shared_ptr<const Node>>(p.begin(), p.end())
  );
}

}}
