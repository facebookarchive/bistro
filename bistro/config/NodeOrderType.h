/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/FBString.h>

namespace facebook { namespace bistro {

enum class NodeOrderType {
  Original,  // However the node source returns them
  Lexicographic,  // Sorted in dictionary order
  Random,
};

NodeOrderType getNodeOrderType(const folly::fbstring& s);

}}
