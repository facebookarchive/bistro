/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/config/Node.h"

using namespace facebook::bistro;
using namespace std;

TEST(TestNode, HandleNoParents) {
  Node n("foo", 1, true);
  EXPECT_EQ("foo", n.name());
  EXPECT_EQ(1, n.level());
  EXPECT_TRUE(n.enabled());
  EXPECT_EQ(Node::ID(0), n.id());

  vector<const Node*> nodes;
  for (const auto& node : n.traverseUp()) {
    nodes.push_back(&node);
  }
  EXPECT_EQ(vector<const Node*>{&n}, nodes);
  EXPECT_EQ(vector<string>{"foo"}, n.getPathToNode());
}

TEST(TestNode, HandleParents) {
  Node n("foo", 1, true);
  Node n2("foo2", 2, true, &n);
  Node n3("foo3", 3, true, &n2);

  EXPECT_EQ(&n2, n3.parent());

  vector<const Node*> nodes;
  for (const auto& node : n3.traverseUp()) {
    nodes.push_back(&node);
  }
  EXPECT_EQ((vector<const Node*>{&n3, &n2, &n}), nodes);
  EXPECT_EQ((vector<string>{"foo", "foo2", "foo3"}), n3.getPathToNode());
}
