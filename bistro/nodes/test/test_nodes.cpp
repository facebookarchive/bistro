/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/nodes/test/utils.h"

using namespace facebook::bistro;
using namespace std;

TEST(TestNodes, CheckIterateOverLevel) {
  Nodes nodes;
  // Test both forms of add()
  vector<NodePtr> nodes_vector = {make_shared<Node>("foo", 1, true)};
  nodes.add(nodes_vector.begin(), nodes_vector.end());
  EXPECT_EQ("bar", nodes.add("bar", 2, true)->name());

  // Test iterating over levels
  for (const auto& node : nodes.iterateOverLevel(0)) {
    EXPECT_EQ(getLocalHostName(), node->name());  // Always made by Nodes
  }
  for (const auto& node : nodes.iterateOverLevel(1)) {
    EXPECT_EQ("foo", node->name());
  }
  for (const auto& node : nodes.iterateOverLevel(2)) {
    EXPECT_EQ("bar", node->name());
  }
  for (const auto& node : nodes.iterateOverLevel(3)) {  // Nonexistent level
    ASSERT_FALSE(true) << "Not reached";
  }

  // Test noninstance node iteration for other tests :)
  int count = 0;
  for (const auto& node : iterate_non_instance_nodes(nodes)) {
    EXPECT_NE("instance", node->name());
    count++;
  }
  EXPECT_EQ(2, count);
}
