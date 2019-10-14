/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

#include "bistro/bistro/utils/PeriodicPoller.h"

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro {

class Config;
class ConfigLoader;
class Nodes;

class NodesLoader {

public:
  NodesLoader(
    std::shared_ptr<ConfigLoader>,
    std::chrono::milliseconds update_period,
    std::chrono::milliseconds retry_period
  );

  std::shared_ptr<const Nodes> getDataOrThrow() const {
    return loader_.getDataOrThrow();
  }

  // This is used in tests. It would be reasonable to remove this and
  // replace it with the following pattern:
  //
  //   NodesLoader nodes_loader(
  //     make_shared<InMemoryConfigLoader>(config),
  //     std::chrono::milliseconds(10),
  //     std::chrono::milliseconds(10)
  //   );
  //   nodes_loader->getDataOrThrow();
  //
  // Not doing it now since it's a lot of mechanical work without benefit.
  static void _fetchNodesImpl(const Config& config, Nodes* nodes);

private:
  // It's not easy to separate fetching from pure post-processing for nodes,
  // since it's a multi-step process.
 static std::shared_ptr<const Nodes> processRawData(
     const std::shared_ptr<const Nodes> /*unused_prev_nodes*/,
     const std::shared_ptr<const Nodes>& nodes) {
   return nodes;
 }

 PeriodicPoller<
     std::shared_ptr<const Nodes>,
     int,
     Nodes,
     NodesLoader::processRawData>
     loader_;
};

}}
