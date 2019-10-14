/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/parsing_common.h"
#include "bistro/bistro/if/gen-cpp2/scheduler_types_custom_protocol.h"
#include "bistro/bistro/thrift/ThriftConversion.h"

using namespace facebook::bistro;
using apache::thrift::debugString;
using folly::dynamic;

std::set<dynamic> setFrom(const dynamic& d) {
  return std::set<dynamic>(d.begin(), d.end());
}

std::set<dynamic> setFrom(const char* s1, const char* s2) {
  return std::set<dynamic>{s1, s2};
}

cpp2::BistroJobConfigFilters mockFilters() {
  cpp2::BistroJobConfigFilters f;
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
  const dynamic errors_d = dynamic::object("fake", "value");

  cpp2::BistroJobConfig c;
  c.name = "job1";
  c.enabled = true;
  c.owner = "owner";
  c.resources["foo_resource"] = 7;
  c.config = "{\"abc\":123}";
  c.priority = 2.0;
  c.filters["fakeLevel"] = mockFilters();
  c.error = folly::toJson(errors_d);
  c.levelForTasks = "fakeLevel";
  c.levelForHostPlacement = "fakeLevel2";
  c.hostPlacement = "fakeHost";
  c.dependsOn = {"jobX", "jobY"};
  c.killOrphanTasksAfterSec_ref() = 0.123;
  // Set some non-default fields, but not all since Job.cpp already tests them
  c.taskSubprocessOptions.pollMs = 666;
  c.taskSubprocessOptions.parentDeathSignal = 555;
  c.killRequest.killWaitMs = 456;
  c.versionID = 987654321;
  // Leave backoff at default. Not testing the other cases, this is deprecated.

  auto d = toDynamic(c);
  ASSERT_EQ(dynamic("job1"), d["name"]);
  ASSERT_TRUE(d["enabled"].asBool());
  ASSERT_EQ(dynamic("owner"), d["owner"]);
  ASSERT_EQ(2.0, d["priority"].asDouble());
  ASSERT_EQ(7, d["resources"]["foo_resource"].asInt());
  ASSERT_EQ(dynamic(dynamic::object("abc", 123)), d["config"]);
  ASSERT_EQ(toDynamic(mockFilters()), d["filters"]["fakeLevel"]);
  ASSERT_EQ(errors_d, d["errors"]);
  ASSERT_EQ(dynamic("fakeLevel"), d["level_for_tasks"]);
  ASSERT_EQ(dynamic("fakeLevel2"), d["level_for_host_placement"]);
  ASSERT_EQ(0.123, d["kill_orphan_tasks_after_sec"].asDouble());
  ASSERT_EQ(666, d[kTaskSubprocess][kPollMs].asInt());
  ASSERT_EQ(555, d[kTaskSubprocess][kParentDeathSignal].asInt());
  ASSERT_EQ(kTerm, d[kKillSubprocess][kMethod].asString());
  ASSERT_EQ(456, d[kKillSubprocess][kKillWaitMs].asInt());
  ASSERT_EQ(987654321, d.at("version_id").asInt());
  ASSERT_EQ(dynamic("fakeHost"), d["host_placement"]);
  ASSERT_EQ(dynamic(dynamic::array("jobX", "jobY")), d["depends_on"]);
  ASSERT_EQ(0, d.count("backoff"));

  // Try parse-and-export for a different presentation of the same Job.
  Config config(dynamic::object
    ("resources", dynamic::object("fakeLevel", dynamic::object
      ("foo_resource", dynamic::object
        ("default", 0)
        ("limit", 0)
      )
    ))
    ("nodes", dynamic::object
      ("levels", dynamic::array("fakeLevel", "fakeLevel2"))
    )
    // We used not to handle "fail" correctly, so test that.
    ("backoff", dynamic::array("fail"))
  );
  Job j(config, "job1", d);
  for (const auto& jd : std::vector<dynamic>{d, j.toDynamic(config)}) {
    auto c2 = toThrift("job1", jd);
    ASSERT_EQ(c.name, c2.name);
    ASSERT_EQ(c.enabled, c2.enabled);
    ASSERT_EQ(c.owner, c2.owner);
    ASSERT_EQ(c.resources, c2.resources);
    ASSERT_EQ(c.config, c2.config);
    ASSERT_EQ(c.priority, c2.priority);
    ASSERT_EQ(c.filters, c2.filters);
    ASSERT_EQ(c.levelForTasks, c2.levelForTasks);
    ASSERT_EQ(c.levelForHostPlacement, c2.levelForHostPlacement);
    ASSERT_EQ(
        c.killOrphanTasksAfterSec_ref().value_unchecked(),
        c2.killOrphanTasksAfterSec_ref().value_unchecked());
    ASSERT_EQ(c.taskSubprocessOptions, c2.taskSubprocessOptions);
    ASSERT_EQ(c.killRequest, c2.killRequest);
    ASSERT_EQ(c.versionID, c2.versionID);
    ASSERT_EQ(c.hostPlacement, c2.hostPlacement);
    ASSERT_EQ(c.dependsOn, c2.dependsOn);
    ASSERT_EQ(c, c2) << debugString(c) << " != " << debugString(c2);
  }
}
