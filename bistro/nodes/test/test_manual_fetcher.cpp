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

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/nodes/test/utils.h"
#include "bistro/bistro/utils/Exception.h"
#include <folly/Conv.h>

using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestManualFetcher, HandleNoSources) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1", "level2"})
      ("node_source", "manual")
      ("node_source_prefs", dynamic::object
        // Neither node can be a source, since they're each other's children.
        ("node1", "node2")
        ("node2", "node1")
      )
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestManualFetcher, HandleNotEnoughLevels) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1", "level2"})
      ("node_source", "manual")
      ("node_source_prefs", dynamic::object
        ("node1", "node2")
        ("node2", "node3")
      )
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestManualFetcher, HandleValid) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1", "level2", "level3"})
      ("node_source", "manual")
      ("node_source_prefs", dynamic::object
        ("node11", {"node111", "node112"})
        ("node1", {"node11", "node12"})
        ("node2", {"node21", "node22"})
      )
    )
  );
  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);

  auto instance = nodes.getInstance();
  auto node1 = nodes.getNodeVerySlow("node1");
  ASSERT_EQ(instance, node1->parent());

  auto node2 = nodes.getNodeVerySlow("node2");
  ASSERT_EQ(instance, node2->parent());

  auto node11 = nodes.getNodeVerySlow("node11");
  ASSERT_EQ(node1.get(), node11->parent());

  auto node112 = nodes.getNodeVerySlow("node112");
  ASSERT_EQ(node11.get(), node112->parent());
}

TEST(TestManualFetcher, HandleOneLevel) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1"})
      ("node_source", "manual")
      ("node_source_prefs", dynamic::object
        ("node1", {})
      )
    )
  );
  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);
  auto it = nodes.begin();
  ASSERT_EQ(2, distance(nodes.begin(), nodes.end()));
  ASSERT_NE(it, nodes.end());
  ASSERT_EQ(nodes.getInstance(), (*it).get());
  ++it;
  ASSERT_NE(it, nodes.end());
  ASSERT_EQ("node1", (*it)->name());
  ++it;
  ASSERT_EQ(it, nodes.end());
}
