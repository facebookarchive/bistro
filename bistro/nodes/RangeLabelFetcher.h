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

class RangeLabelFetcher : public NodeFetcher {

public:
 void fetch(const Config&, const NodeConfig&, Nodes*) const override;
};

}}
