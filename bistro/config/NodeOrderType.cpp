/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/NodeOrderType.h"

#include <unordered_map>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

namespace {
std::unordered_map<folly::fbstring, NodeOrderType> type_map = {
  { "original", NodeOrderType::Original },
  { "lexicographic", NodeOrderType::Lexicographic },
  { "random", NodeOrderType::Random },
};
}  // anonymous namespace

NodeOrderType getNodeOrderType(
    const folly::fbstring& s) {

  auto it = type_map.find(s);
  if (it == type_map.end()) {
    throw BistroException("Unknown node_order type: ", s);
  }
  return it->second;
}

}}
