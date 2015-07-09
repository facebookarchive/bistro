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

#include "bistro/bistro/thrift/ThriftConversion.h"

using namespace facebook::bistro;
using namespace facebook::bistro::cpp2;
using namespace std;
using folly::dynamic;

set<dynamic> setFrom(const dynamic& d) {
  return set<dynamic>(d.begin(), d.end());
}

set<dynamic> setFrom(const char* s1, const char* s2) {
  return set<dynamic>{s1, s2};
}

BistroJobConfigFilters mockFilters() {
  BistroJobConfigFilters f;
  f.whitelist = { "abc", "def" };
  f.whitelistRegex = "whitelist_regex";
  f.blacklist = { "foo", "bar" };
  f.blacklistRegex = "blacklist_regex";
  f.tagWhitelist = { "tag1", "tag2" };
  f.fractionOfNodes = 0.75;
  return f;
}

TEST(TestThriftConversion, HandleJobFilters) {
  auto f = mockFilters();
  auto d = toDynamic(f);
  ASSERT_EQ(setFrom("abc", "def"), setFrom(d["whitelist"]));
  ASSERT_EQ(dynamic("whitelist_regex"), d["whitelist_regex"]);
  ASSERT_EQ(setFrom("bar", "foo"), setFrom(d["blacklist"]));
  ASSERT_EQ(dynamic("blacklist_regex"), d["blacklist_regex"]);
  ASSERT_EQ(0.75, d["fraction_of_nodes"].asDouble());
  ASSERT_EQ(setFrom("tag1", "tag2"), setFrom(d["tag_whitelist"]));

  auto f2 = toThrift(d);
  ASSERT_EQ(f.whitelist, f2.whitelist);
  ASSERT_EQ(f.whitelistRegex, f2.whitelistRegex);
  ASSERT_EQ(f.blacklist, f2.blacklist);
  ASSERT_EQ(f.blacklistRegex, f2.blacklistRegex);
  ASSERT_EQ(f.tagWhitelist, f2.tagWhitelist);
  ASSERT_EQ(f.fractionOfNodes, f2.fractionOfNodes);
  ASSERT_EQ(f, f2);
}

TEST(TestThriftConversion, HandleJobFiltersPartial) {
  cpp2::BistroJobConfigFilters conf;
  conf.whitelist = { "abc", "def" }; // only set this one
  auto d = toDynamic(conf);

  ASSERT_EQ(setFrom("abc", "def"), setFrom(d["whitelist"]));

  // The rest shouldn't be set
  EXPECT_EQ(d.find("whitelist_regex"), d.items().end());
  EXPECT_EQ(d.find("blacklist"), d.items().end());
  EXPECT_EQ(d.find("blacklist_regex"), d.items().end());
}

TEST(TestThriftConversion, HandleJob) {
  BistroJobConfig c;
  c.name = "job1";
  c.enabled = true;
  c.owner = "owner";
  c.resources["foo_resource"] = 7;
  c.config = "{\"abc\":123}";
  c.priority = 2.0;
  c.filters["level1"] = mockFilters();
  c.error = "err1";
  c.levelForTasks = "fakeLevel";
  c.levelForHostPlacement = "fakeLevel2";
  c.hostPlacement = "fakeHost";
  c.dependsOn = {"jobX", "jobY"};
  c.killOrphanTasksAfterSec = 0.123;
  c.__isset.killOrphanTasksAfterSec = true;
  c.versionID = 987654321;

  auto d = toDynamic(c);
  ASSERT_EQ(dynamic("job1"), d["name"]);
  ASSERT_EQ(true, d["enabled"].asBool());
  ASSERT_EQ(dynamic("owner"), d["owner"]);
  ASSERT_EQ(2.0, d["priority"].asDouble());
  ASSERT_EQ(7, d["resources"]["foo_resource"].asInt());
  ASSERT_EQ(dynamic(dynamic::object("abc", 123)), d["config"]);
  ASSERT_EQ(toDynamic(mockFilters()), d["filters"]["level1"]);
  ASSERT_EQ(dynamic("err1"), d["error"]);
  ASSERT_EQ(dynamic("fakeLevel"), d["level_for_tasks"]);
  ASSERT_EQ(dynamic("fakeLevel2"), d["level_for_host_placement"]);
  ASSERT_EQ(0.123, d["kill_orphan_tasks_after_sec"].asDouble());
  ASSERT_EQ(987654321, d.at("version_id").asInt());
  ASSERT_EQ(dynamic("fakeHost"), d["host_placement"]);
  ASSERT_EQ(dynamic({"jobX", "jobY"}), d["depends_on"]);

  auto c2 = toThrift("job1", d);
  ASSERT_EQ(c.name, c2.name);
  ASSERT_EQ(c.enabled, c2.enabled);
  ASSERT_EQ(c.owner, c2.owner);
  ASSERT_EQ(c.resources, c2.resources);
  ASSERT_EQ(c.config, c2.config);
  ASSERT_EQ(c.priority, c2.priority);
  ASSERT_EQ(c.filters, c2.filters);
  ASSERT_EQ(c.levelForTasks, c2.levelForTasks);
  ASSERT_EQ(c.levelForHostPlacement, c2.levelForHostPlacement);
  ASSERT_EQ(c.killOrphanTasksAfterSec, c2.killOrphanTasksAfterSec);
  ASSERT_EQ(c.versionID, c2.versionID);
  ASSERT_EQ(c.hostPlacement, c2.hostPlacement);
  ASSERT_EQ(c.dependsOn, c2.dependsOn);
  ASSERT_EQ(c, c2);
}
