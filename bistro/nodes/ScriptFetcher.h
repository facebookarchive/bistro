/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/nodes/NodeFetcher.h"

namespace facebook { namespace bistro {

class Nodes;

/**
 * This node fetcher generates nodes using a user-provided script.  Each stdout
 * line of the script corresponds to a node name at the specified level.
 *
 * We pass the parent node name to the script as argv[1], so you may generate
 * different node names based on the parent.
 *
 */
class ScriptFetcher : public NodeFetcher {

public:
 void fetch(const Config&, const NodeConfig&, Nodes*) const override;
};

}}
