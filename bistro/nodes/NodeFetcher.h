/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace facebook { namespace bistro {

class Config;
class NodeConfig;
class Nodes;

class NodeFetcher {

public:
  // Most fetchers should be stateless. If you need a non-const variant of
  // this, please try not to force statefulness onto the other fetchers.
  virtual void fetch(
    const Config& config,
    const NodeConfig& node_config,
    Nodes* all_nodes
  ) const = 0;

  virtual ~NodeFetcher() = 0;

  static NodeFetcher* create(const std::string& source);

  /**
   * Add a new node fetcher, as a plugin. Should be called before we create a
   * config loader.
   */
  static void add(const std::string& label, std::shared_ptr<NodeFetcher> f);

};

}}
