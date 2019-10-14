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
#include "bistro/bistro/nodes/test/utils.h"
#include "bistro/bistro/utils/TemporaryFile.h"

using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestScriptFetcher, HandleNoScript) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "script")
        ("prefs", dynamic::object)
      ))
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestScriptFetcher, HandleSimple) {
  TemporaryDir tmp_dir;
  TemporaryFile cmdFile(tmp_dir.createFile());
  cmdFile.writeString(
    "#!/bin/sh\n"
    "echo foo\n"
    "echo bar\n"
  );
  PCHECK(chmod(cmdFile.getFilename().c_str(), 0700) == 0);

  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "script")
        ("prefs", dynamic::object
          ("parent_level", "instance")
          ("script", cmdFile.getFilename().native())
        )
      ))
    )
  );
  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);
  auto it = nodes.begin();
  ASSERT_EQ(nodes.getInstance(), (*it).get());
  ++it;
  ASSERT_NE(it, nodes.end());
  ASSERT_EQ("foo", (*it)->name());
  ASSERT_EQ(nodes.getInstance(), (*it)->parent());
  ++it;
  ASSERT_NE(it, nodes.end());
  ASSERT_EQ("bar", (*it)->name());
  ASSERT_EQ(nodes.getInstance(), (*it)->parent());
  ++it;
  ASSERT_EQ(it, nodes.end());
}

TEST(TestScriptFetcher, HandleParentArgument) {
  TemporaryDir tmp_dir;
  TemporaryFile cmdFile(tmp_dir.createFile());
  cmdFile.writeString(
    "#!/bin/sh\n"
    "echo $1:foo\n"
  );
  PCHECK(chmod(cmdFile.getFilename().c_str(), 0700) == 0);

  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(
        dynamic::object
          ("source", "manual")
          ("prefs", dynamic::object
            ("node1", dynamic::array())
            ("node2", dynamic::array())
          )
        ,
        dynamic::object
          ("source", "script")
          ("prefs", dynamic::object
            ("parent_level", "level1")
            ("script", cmdFile.getFilename().native())
          )
      ))
    )
  );
  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);

  ASSERT_EQ(5, nodes.size());
  auto n1 = getNodeVerySlow(nodes, "node1:foo");
  ASSERT_EQ(2, n1->level());
  ASSERT_EQ("node1", n1->parent()->name());
  auto n2 = getNodeVerySlow(nodes, "node2:foo");
  ASSERT_EQ(2, n2->level());
  ASSERT_EQ("node2", n2->parent()->name());
}
