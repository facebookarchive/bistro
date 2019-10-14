/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/ScriptFetcher.h"

#include <iostream>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/utils.h"
#include <folly/Subprocess.h>
#include <folly/gen/File.h>


namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

void ScriptFetcher::fetch(
    const Config& config,
    const NodeConfig& node_config,
    Nodes *all_nodes) const {

  // Use the "parent_level" pref to get my parents and my level ID.
  auto my_level_and_parents =
    getMyLevelAndParents(config, node_config, all_nodes);
  for (const auto& parent : my_level_and_parents.second) {
    Subprocess p(
      vector<string>{
        node_config.prefs.requireConvert<string>("script"),
        parent->name()
      },
      Subprocess::Options().pipeStdout()
    );
    gen::byLine(p.stdoutFd()) | [&](StringPiece line) {
      all_nodes->add(
        line.str(),
        my_level_and_parents.first,
        true,
        parent.get()
      );
      std::cout << line << std::endl;
    };
    p.wait();
  }
}

}}
