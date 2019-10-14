/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/nodes/NodeFetcher.h"

#include <chrono>
#include <unordered_map>

#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/AddTimeFetcher.h"
#include "bistro/bistro/nodes/EmptyFetcher.h"
#include "bistro/bistro/nodes/ManualFetcher.h"
#include "bistro/bistro/nodes/RangeLabelFetcher.h"
#include "bistro/bistro/nodes/ScriptFetcher.h"
#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

using namespace std;

namespace {
unordered_map<string, shared_ptr<NodeFetcher>> node_fetchers = {
  {
    "add_time",
    shared_ptr<NodeFetcher>(new AddTimeFetcher<chrono::system_clock>())
  },
  { "empty", shared_ptr<NodeFetcher>(new EmptyFetcher()) },
  { "manual", shared_ptr<NodeFetcher>(new ManualFetcher()) },
  { "range_label", shared_ptr<NodeFetcher>(new RangeLabelFetcher()) },
  { "script", shared_ptr<NodeFetcher>(new ScriptFetcher()) },
};
}

NodeFetcher::~NodeFetcher() {
}

NodeFetcher* NodeFetcher::create(const string& source) {
  auto it = node_fetchers.find(source);
  if (it == node_fetchers.end()) {
    throw BistroException("Invalid node source: ", source);
  }
  return it->second.get();
}

void NodeFetcher::add(const string& label, shared_ptr<NodeFetcher> f) {
  if (node_fetchers.count(label) > 0) {
    throw BistroException("Duplicate node source: ", label);
  }
  node_fetchers[label] = f;
}

}}
