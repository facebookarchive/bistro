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

TEST(TestRangeLabelFetcher, MissingParent) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("my_level"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "range_label")
        ("prefs", dynamic::object("start", "1")("end", "2"))
      ))
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestRangeLabelFetcher, InvalidParent) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("my_level"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "range_label")
        ("prefs",
          dynamic::object("start", "1")("end", "2")("parent_level", "bork")
        )
      ))
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

TEST(TestRangeLabelFetcher, TooFewLevels) {
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array())
      ("node_sources", dynamic::array(dynamic::object
        ("source", "range_label")
        ("prefs",
          dynamic::object("start", "1")("end", "2")("parent_level", "instance")
        )
      ))
    )
  );
  Nodes nodes;
  EXPECT_THROW(NodesLoader::_fetchNodesImpl(config, &nodes), runtime_error);
}

void check_simple(dynamic&& prefs, const vector<string>& exp_names) {
  prefs["parent_level"] = "instance";
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("my_level"))
      ("node_sources", dynamic::array(dynamic::object
        ("source", "range_label")
        ("prefs", prefs)
      ))
    )
  );

  Nodes nodes;
  NodesLoader::_fetchNodesImpl(config, &nodes);
  vector<string> names;

  for (const auto &node : iterate_non_instance_nodes(nodes)) {
    EXPECT_EQ(1, node->level());
    EXPECT_EQ(nodes.getInstance(), node->parent());

    names.push_back(node->name());
  }
  EXPECT_EQ(exp_names, names);
}

TEST(TestRangeLabelFetcher, NoFormat) {
  check_simple(
    dynamic::object("start", "3")("end", "5"),
    vector<string>{
      to<string>(getLocalHostName(), "__3"),
      to<string>(getLocalHostName(), "__4"),
      to<string>(getLocalHostName(), "__5")
    }
  );
}

TEST(TestRangeLabelFetcher, SimpleFormat) {
  check_simple(
    dynamic::object("start", "-1")("end", "1")("format", "c{i}"),
    vector<string>{"c-1", "c0", "c1"}
  );
}

TEST(TestRangeLabelFetcher, FullFormat) {
  check_simple(
    dynamic::object
      ("start", "2")("end", "3")("format", "{parent}_{i}_{start}_{end}"),
    vector<string>{
      to<string>(getLocalHostName(), "_2_2_3"),
      to<string>(getLocalHostName(), "_3_2_3")
    }
  );
}

TEST(TestRangeLabelFetcher, EmptyRange) {
  check_simple(dynamic::object("start", "3")("end", "2"), vector<string>{});
}

Nodes fetch_two_levels(string s1, string e1, string s2, string e2) {
  Nodes nodes;
  dynamic sources = dynamic::array(
    dynamic::object
      ("source", "range_label")
      ("prefs", dynamic::object
        ("start", s1)("end", e1)("parent_level", "instance")
      ),
    dynamic::object
      ("source", "range_label")
      ("prefs", dynamic::object
        ("start", s2)("end", e2)("parent_level", "mid_level")
        ("enabled", "false")
      )
  );
  Config config(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", dynamic::array("mid_level", "bottom_level"))
      ("node_sources", sources)
    )
  );
  NodesLoader::_fetchNodesImpl(config, &nodes);
  return nodes;
}

TEST(TestRangeLabelFetcher, EmptyParents) {
  Nodes nodes = fetch_two_levels("5", "3", "3", "5");
  EXPECT_EQ(1, nodes.size());
}

TEST(TestRangeLabelFetcher, MultiLevel) {
  Nodes nodes = fetch_two_levels("1", "2", "3", "4");
  EXPECT_EQ(7, nodes.size());

  // Spot-check some nodes
  vector<std::shared_ptr<const Node>> nodes_vec(nodes.begin(), nodes.end());
  const auto& mid = nodes_vec[2], bottom = nodes_vec.back();

  EXPECT_EQ(1, mid->level());
  EXPECT_EQ(to<string>(getLocalHostName(), "__2"), mid->name());
  EXPECT_EQ(nodes.getInstance(), mid->parent());
  EXPECT_TRUE(mid->enabled());

  EXPECT_EQ(2, bottom->level());
  EXPECT_EQ(to<string>(getLocalHostName(), "__2__4"), bottom->name());
  EXPECT_EQ(mid.get(), bottom->parent());
  EXPECT_FALSE(bottom->enabled());
}
