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
#include "bistro/bistro/nodes/AddTimeFetcher.h"
#include "bistro/bistro/nodes/NodeFetcher.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/nodes/test/utils.h"
#include "bistro/bistro/utils/Exception.h"
#include <folly/json.h>

using namespace facebook::bistro;
using namespace folly;
using namespace std;

// This makes the test far more readable, and I don't know of a better way
// to do this, since object isn't a type.  Please don't hate.
#define obj dynamic::object

struct StubClock {
  typedef std::chrono::seconds duration;

  static std::chrono::time_point<StubClock> now() {
    return
      std::chrono::time_point<StubClock>(std::chrono::duration<int64_t>(t));
  }
  static int64_t t;
};
int64_t StubClock::t = 0;

void fetch(double t, initializer_list<dynamic> items, Nodes *nodes) {
  static bool initialized = false;
  if (!initialized){
    NodeFetcher::add(
      "test_add_time", shared_ptr<NodeFetcher>(new AddTimeFetcher<StubClock>{})
    );
    initialized = true;
  }

  StubClock::t = t;

  Config config{obj
    ("resources", obj)
    ("nodes", obj
      ("levels", { "my_level" })
      ("node_sources", {
        obj
          ("source", "test_add_time")
          ("prefs", obj
            ("parent_level", "instance")
            ("schedule", items)
          ),
      })
    )
  };
  NodesLoader::_fetchNodesImpl(config, nodes);
}

void check_schedule(  // Negative expected times mean "disabled node"
  time_t cur_time, initializer_list<dynamic> items, vector<time_t> exp_times,
  vector<unordered_set<string>> exp_tags = {}
) {
  Nodes nodes;
  fetch(cur_time, items, &nodes);

  vector<string> exp_names;
  vector<bool> exp_enabled;
  for (auto t : exp_times) {
    exp_names.emplace_back(
      format("{}:{}", nodes.getInstance()->name(), abs(t)).str()
    );
    exp_enabled.push_back(t >= 0);
  }

  // Must sort nodes for determinism; string sort is good enough
  auto nodes_iter = iterate_non_instance_nodes(nodes);
  vector<NodePtr> sorted_nodes{begin(nodes_iter), end(nodes_iter)};
  sort(begin(sorted_nodes), end(sorted_nodes), [](NodePtr a, NodePtr b){
    return a->name() < b->name();
  });

  vector<string> names;
  vector<bool> enabled;
  vector<unordered_set<string>> tags;
  for (const auto &node : sorted_nodes) {
    EXPECT_EQ(1, node->level());
    EXPECT_EQ(nodes.getInstance(), node->parent());
    names.push_back(node->name());
    enabled.push_back(node->enabled());
    tags.push_back(node->tags());
  }
  EXPECT_EQ(exp_names, names);
  EXPECT_EQ(exp_enabled, enabled);
  if (!exp_tags.size()) {
    exp_tags.resize(tags.size());  // Default to all tags empty
  }
  EXPECT_EQ(exp_tags, tags);
}

TEST(TestAddTimeFetcher, TestLifetimeMandatory) {
  EXPECT_THROW(
    check_schedule(4, {obj("cron", obj("epoch", obj("period", 4)))}, {4}),
    runtime_error
  );
}

TEST(TestAddTimeFetcher, TestLifetime) {
  // A node with a lifetime of 1 second
  dynamic i4_l1 = obj("lifetime", 1)("cron", obj("epoch", obj("period", 4)));
  check_schedule(4, {i4_l1}, {4});
  check_schedule(5, {i4_l1}, {});  // Expires 1 second later
  check_schedule(7, {i4_l1}, {});
  check_schedule(8, {i4_l1}, {8});  // And comes back by second 8

  // A node with a lifetime of 3 seconds
  dynamic i4_l3 = obj("lifetime", 3)("cron", obj("epoch", obj("period", 4)));
  check_schedule(4, {i4_l3}, {4});
  check_schedule(6, {i4_l3}, {4});  // Still around at 2 seconds
  check_schedule(7, {i4_l3}, {});   // Gone
  check_schedule(8, {i4_l3}, {8});  // Resurrected
}

TEST(TestAddTimeFetcher, TestEnabledLifetime) {
  // Enabled lifetime of 3 seconds
  dynamic i4_l4_el3 = obj
    ("lifetime", 4)
    ("enabled_lifetime", 3)
    ("cron", obj("epoch", obj("period", 4)));
  check_schedule(4, {i4_l4_el3}, {4});
  check_schedule(6, {i4_l4_el3}, {4});   // Still enabled 2 seconds in
  check_schedule(7, {i4_l4_el3}, {-4});  // Disabled at 3
  check_schedule(8, {i4_l4_el3}, {8});   // Replaced by the next timestamp
}

TEST(TestAddTimeFetcher, TestRealistic) {
  for (time_t t : {0, 5, 77, 89}) {
    check_schedule(
      990 + t,
      {obj
        ("lifetime", 720)
        ("enabled_lifetime", 360)
        ("cron", obj("epoch", obj("period", 90)))},
      {-360, -450, -540, -630, 720, 810, 900, 990}
    );
  }
}

TEST(TestAddTimeFetcher, TestVaried) {
  dynamic i7_l7 = obj("lifetime", 7)("cron", obj("epoch", obj("period", 7)));
  check_schedule(10, {i7_l7}, {7});
  // Test cases near cur_time % period == 0
  dynamic i8_l8 = obj("lifetime", 8)("cron", obj("epoch", obj("period", 8)));
  check_schedule(7, {i8_l8}, {0});
  check_schedule(8, {i8_l8}, {8});
  check_schedule(9, {i8_l8}, {8});
  // A positive lifetime means you are alive at the 'period' tick.
  for (int l : {1, 2}) {
    dynamic i2 = obj("lifetime", l)("cron", obj("epoch", obj("period", 2)));
    check_schedule(6, {i2}, {6});
  }
  // Lifetime can exceed period
  dynamic i2_l3 = obj("lifetime", 3)("cron", obj("epoch", obj("period", 2)));
  check_schedule(6, {i2_l3}, {4, 6});
  // Test offset
  dynamic i6_l6_o2 = obj
    ("lifetime", 6)("cron", obj("epoch", obj("period", 6)("start", 2)));
  check_schedule(9, {i6_l6_o2}, {8});
  dynamic i6_l8_o2 = obj
    ("lifetime", 8)("cron", obj("epoch", obj("period", 6)("start", 2)));
  check_schedule(9, {i6_l8_o2}, {2, 8});
}

TEST(TestAddTimeFetcher, TestMultiItemWithTags) {
  check_schedule(
    30,
    {
      obj
        ("lifetime", 8)
        ("tags", {"x"})
        ("cron", obj("epoch", obj("period", 2)("start", 1))),
      obj
        ("lifetime", 6)
        ("tags", {"y"})
        ("cron", obj("epoch", obj("period", 3)("start", 2))),
    },
    {23, 25, 26, 27, 29},
    {{"x"}, {"x"}, {"y"}, {"x"}, {"x", "y"}}
  );
}

TEST(TestAddTimeFetcher, TestStandardCron) {
  // This is covered in far more detail by Cron's own tests. Here we just
  // check that it can be invoked instead of epoch.
  boost::local_time::time_zone_ptr local_tz;
  setenv("TZ", "PST+8PDT,M3.2.0,M11.1.0", 1);  // Make the test deterministic
  tzset();
  check_schedule(
    50000, // Thu Jan  1 05:53:20 PST 1970
    {obj
      ("lifetime", 14400)  // 4 hours -- anything after 01:53:20
      ("enabled_lifetime", 900)  // 15 minutes -- anything after 05:38:20
      ("cron", obj  // disabled 1:58 & 5:33, enabled 5:47
        ("minute", {33, 47, 58})
        ("hour", obj("start", 1)("period", 4))
        ("dst_fixes", {"skip", "repeat_use_both"})  // required but irrelevant
      )
    },
    {-35880, -48780, 49620}
  );
}
