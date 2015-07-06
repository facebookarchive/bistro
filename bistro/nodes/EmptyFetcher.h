#pragma once

#include "bistro/bistro/nodes/NodeFetcher.h"

namespace facebook { namespace bistro {

class Nodes;

/**
 * Fetches no nodes. Good for deployments that don't have nodes.
 */
class EmptyFetcher : public NodeFetcher {

public:
 void fetch(const Config&, const NodeConfig&, Nodes*) const override {}
};

}}
