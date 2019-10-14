/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/NodesLoader.h"

#include "bistro/bistro/config/ConfigLoader.h"
#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/Nodes.h"
#include <folly/experimental/AutoTimer.h>
#include "bistro/bistro/nodes/NodeFetcher.h"

namespace facebook { namespace bistro {

using namespace std;

NodesLoader::NodesLoader(
    shared_ptr<ConfigLoader> config_loader,
    std::chrono::milliseconds update_period,
    std::chrono::milliseconds retry_period)
    : loader_(
          "NodesLoader",
          [config_loader](
              std::shared_ptr<const Nodes>* out_nodes,
              int* /*ignored_state*/,
              std::shared_ptr<const Nodes> /*unused_prev_nodes*/
              ) {
            auto nodes = make_shared<Nodes>();
            // This briefly echos ConfigLoader errors after they resolve.
            // That's ok.
            _fetchNodesImpl(*config_loader->getDataOrThrow(), nodes.get());
            *out_nodes = nodes;
            // For now, there's no benefit to detecting whether the nodes
            // changed.
            return true;
          },
          update_period,
          retry_period) {}

void NodesLoader::_fetchNodesImpl(const Config& config, Nodes* nodes) {
  folly::AutoTimer<> timer;
  for (const auto& settings : config.nodeConfigs) {
    NodeFetcher::create(settings.source)->fetch(config, settings, nodes);
    timer.log("Have ", nodes->size(), " nodes after ", settings.source);
  }
}

}}
