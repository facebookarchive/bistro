/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "manual")
        // Neither node can be a source, since they're each other's children.
        ("prefs", dynamic::object("node1", "node2")("node2", "node1"))
      ))
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestManualFetcher, HandleNotEnoughLevels) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "manual")
        ("prefs", dynamic::object("node1", "node2")("node2", "node3"))
      ))
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestManualFetcher, HandleValid) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1", "level2", "level3"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "manual")
        ("prefs", dynamic::object
          ("node11", dynamic::array("node111", "node112"))
          ("node1", dynamic::array("node11", "node12"))
          ("node2", dynamic::array("node21", "node22"))
        )
      ))
    )
  );
  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);

  auto instance = nodes.getInstance();
  auto node1 = getNodeVerySlow(nodes, "node1");
  ASSERT_EQ(instance, node1->parent());

  auto node2 = getNodeVerySlow(nodes, "node2");
  ASSERT_EQ(instance, node2->parent());

  auto node11 = getNodeVerySlow(nodes, "node11");
  ASSERT_EQ(node1.get(), node11->parent());

  auto node112 = getNodeVerySlow(nodes, "node112");
  ASSERT_EQ(node11.get(), node112->parent());
}

TEST(TestManualFetcher, HandleOneLevel) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "manual")
        ("prefs", dynamic::object("node1", dynamic::array()))
      ))
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

TEST(TestManualFetcher, HandleDisabledAndChildren) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "manual")
        ("prefs", dynamic::object
          ("node1", dynamic::object
            ("children", dynamic::array("node11", "node12"))
            ("disabled", true)
          )
          ("node12", dynamic::object("disabled", true))
        )
      ))
    )
  );
  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);

  auto node1 = getNodeVerySlow(nodes, "node1");
  ASSERT_FALSE(node1->enabled());
  ASSERT_EQ(nodes.getInstance(), node1->parent());

  auto node11 = getNodeVerySlow(nodes, "node11");
  ASSERT_TRUE(node11->enabled());
  ASSERT_EQ(node1.get(), node11->parent());

  auto node12 = getNodeVerySlow(nodes, "node12");
  ASSERT_FALSE(node12->enabled());
  ASSERT_EQ(node1.get(), node12->parent());
}
