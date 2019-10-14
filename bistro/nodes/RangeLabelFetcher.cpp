/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/RangeLabelFetcher.h"

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/utils.h"

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

void RangeLabelFetcher::fetch(
    const Config& config,
    const NodeConfig& node_config,
    Nodes *all_nodes) const {
  const auto& prefs = node_config.prefs;

  const string& format_str = prefs.convert<string>("format", "{parent}__{i}");
  const int start = prefs.requireConvert<int>("start");
  const int end = prefs.requireConvert<int>("end");
  const bool enabled = prefs.convert<bool>("enabled", true);

  // These can be used in "format" as {start}, etc.
  map<string, string> format_args {
    {"start", to<string>(start)},
    {"end", to<string>(end)},
  };

  // Use the "parent_level" pref to get my parents and my level ID.
  auto my_level_and_parents =
    getMyLevelAndParents(config, node_config, all_nodes);
  for (const auto& parent : my_level_and_parents.second) {
    format_args["parent"] = parent->name();
    for (int i = start; i <= end; ++i) {
      format_args["i"] = to<string>(i);
      all_nodes->add(
        vformat(format_str, format_args).str(),
        my_level_and_parents.first,
        enabled,
        parent.get()
      );
    }
  }
}

}}
