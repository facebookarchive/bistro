/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/ManualFetcher.h"

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/Nodes.h"
#include <folly/String.h>

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {

struct ManualNodeState {
  int maxLevel_;
  Nodes* allNodes_;
  unordered_map<string, vector<string>> parentToChildren_;
  unordered_set<fbstring> isDisabled_;

  void recursiveAdd(const string& name, const Node* parent, int level) const {
    if (level > maxLevel_) {
      throw BistroException(
         "Node ", name, " would be at level ", level, " but the maximum is ",
         maxLevel_
      );
    }
    const Node* node_ptr = allNodes_->add(
      name, level, isDisabled_.count(name) == 0, parent
    );
    auto it = parentToChildren_.find(name);
    if (it != parentToChildren_.end()) {
      for (const auto& child_name : it->second) {
        recursiveAdd(child_name, node_ptr, level + 1);
      }
    }
  }
};

}  // anonymous namespace

void ManualFetcher::fetch(
    const Config& config,
    const NodeConfig& node_config,
    Nodes* all_nodes) const {

  ManualNodeState state;
  state.allNodes_ = all_nodes;
  state.maxLevel_ = config.getNumConfiguredLevels();
  unordered_set<string> has_parent;
  for (const auto& pair : node_config.prefs) {
    auto& child_names = state.parentToChildren_[pair.first];

    if (pair.second.isString()) {  // A string is a single node name
      child_names.emplace_back(
        pair.second.asString()
      );
    } else if (pair.second.isObject()) {
      if (auto* children = pair.second.get_ptr("children")) {
        for (const auto& kid_name : *children) {
          child_names.emplace_back(kid_name.asString());
        }
      }
      if (pair.second.get_ptr("disabled")) {
        state.isDisabled_.insert(pair.first);
      }
    } else {  // Otherwise it's a list of child names
      for (const auto& kid_name : pair.second) {
        child_names.emplace_back(kid_name.asString());
      }
    }
    for (const auto& child : child_names) {
      has_parent.insert(child);
    }
  }

  // Add the nodes without parents as the children of the instance node
  int num_sources = 0;
  for (const auto& pair : node_config.prefs) {
    if (has_parent.count(pair.first) == 0) {
      state.recursiveAdd(pair.first, all_nodes->getInstance(), 1);
      ++num_sources;
    }
  }

  // This detects bad configs -- if you need no nodes below the instance,
  // don't specify any fetchers.
  if (num_sources == 0 ) {
    throw BistroException("No top level nodes are defined");
  }
}

}}
