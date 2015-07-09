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
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/utils.h"
#include "bistro/bistro/config/Node.h"
#include <folly/Conv.h>
#include <folly/json.h>

using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestJob, HandleAll) {
  dynamic cd1 = dynamic::object
    ("resources", dynamic::object
      ("instance", dynamic::object
        ("concurrency", dynamic::object
          ("default", 1)
          ("limit", 10)
        )
      )
    )
    ("nodes", dynamic::object
      ("levels", {"level1", "level2"})
      ("node_source", "range_label")
    );
  Config c(cd1);
  dynamic jd = dynamic::object
    ("enabled", false)
    ("owner", "owner")
    ("priority", 0.5)
    ("resources", dynamic::object("concurrency", 2))
    ("config", dynamic::object("key", "val"))
    ("filters", dynamic::object
      ("level2", dynamic::object
        ("whitelist_regex", "abc")
      )
    )
    ("depends_on", {"job1", "job2"})
    ("version_id", 123456789)
  ;

  Job j(c, "foo_job", jd);
  EXPECT_EQ(0, j.id());
  EXPECT_EQ("foo_job", j.name());
  EXPECT_TRUE(j.isValid());
  EXPECT_FALSE(j.isEnabled());
  EXPECT_EQ(jd["config"], j.config());
  EXPECT_EQ(ResourceVector{2}, j.resources());
  // Starting from 1 because we had just inserted foo_job into JobNameTable
  EXPECT_EQ(j.dependencies(), vector<Job::ID>({Job::ID(1), Job::ID(2)}));

  Job j2(c, "foo_job", jd);
  auto jd_new = j.toDynamic(c);
  EXPECT_EQ(jd_new, j2.toDynamic(c));

  EXPECT_EQ(123456789, jd_new.at("version_id").asInt());

  // Check the "kill orphans" option -- first, inherited, then overridden.
  EXPECT_FALSE(j.killOrphanTasksAfter().hasValue());
  {
    cd1["kill_orphan_tasks_after_sec"] = false;
    Job jh1(Config(cd1), "bar_job", jd);
    EXPECT_FALSE(jh1.killOrphanTasksAfter().hasValue());
  }
  {
    cd1["kill_orphan_tasks_after_sec"] = true;
    Job jh1(Config(cd1), "bar_job", jd);
    EXPECT_EQ(std::chrono::seconds(0), jh1.killOrphanTasksAfter().value());
  }
  {
    jd["kill_orphan_tasks_after_sec"] = 0.37;
    Job jh1(Config(cd1), "bar_job", jd);
    EXPECT_EQ(
      std::chrono::milliseconds(370), jh1.killOrphanTasksAfter().value()
    );
  }

  // Check default, invalid, and valid level_for_host_placement.
  EXPECT_EQ(StringTable::NotFound, j.levelForHostPlacement());
  {
    jd["level_for_host_placement"] = "not a level";
    Job jh1(c, "bar_job", jd);
    EXPECT_FALSE(jh1.isValid());
    EXPECT_PRED1([](const string& s){
      return s.find("Bad level_for_host_placement") != string::npos;
    }, jh1.error());
  }
  // Cannot have both level_for_host_placement and host_placement
  EXPECT_EQ(StringTable::NotFound, j.levelForHostPlacement());
  {
    jd["level_for_host_placement"] = "level2";
    jd["host_placement"] = "some.host";
    Job jh1(c, "bar_job", jd);
    EXPECT_FALSE(jh1.isValid());
    EXPECT_PRED1([](const string& s){
      return s.find("makes no sense to specify both") != string::npos;
    }, jh1.error());
  }
  // Valid to have just host_placement
  {
    jd.erase("level_for_host_placement");
    Job jh2(c, "bar_job", jd);
    EXPECT_TRUE(jh2.isValid());
    EXPECT_EQ(StringTable::NotFound, jh2.levelForHostPlacement());
    EXPECT_EQ("some.host", jh2.hostPlacement());
  }
  // Also valid to have just level_for_host_placement.
  // Return jd to a working state so we can test level_for_tasks next.
  {
    jd.erase("host_placement");
    jd["level_for_host_placement"] = "level2";
    Job jh2(c, "bar_job", jd);
    EXPECT_TRUE(jh2.isValid());
    EXPECT_EQ(2, jh2.levelForHostPlacement());
    EXPECT_EQ("", jh2.hostPlacement());
  }

  // Defaults to the lowest non-worker level
  EXPECT_EQ(2, j.levelForTasks());
  cd1["level_for_tasks"] = "level2";  // Make the default explicit
  {
    Config ct(cd1);
    EXPECT_EQ(2, Job(ct, "foo_job", jd).levelForTasks());
  }
  cd1["level_for_tasks"] = "level1";  // Change the Config and the Job changes
  {
    Config ct(cd1);
    EXPECT_EQ(1, Job(ct, "foo_job", jd).levelForTasks());
    jd["level_for_tasks"] = "instance";  // But the Job has the final say
    EXPECT_EQ(0, Job(ct, "foo_job", jd).levelForTasks());
    // Error on invalid level
    jd["level_for_tasks"] = "invalid_level";
    Job je(ct, "foo_job", jd);
    EXPECT_FALSE(je.isEnabled());
    EXPECT_FALSE(je.isValid());
    EXPECT_PRED1([](const string& s){
      return s.find("Bad level_for_tasks") != string::npos;
    }, je.error());
  }
}

TEST(TestJob, HandleError) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1", "level2"})
      ("node_source", "range_label")
    )
  );
  dynamic d = dynamic::object
    ("owner", "owner")
    ("config", dynamic::object("key", "val"))
    ("filters", dynamic::object
      ("invalid_level", dynamic::object
        ("whitelist_regex", "abc")
      )
    )
  ;

  Job j(c, "foo_job", d);
  EXPECT_FALSE(j.isValid());
  EXPECT_FALSE(j.isEnabled());
}

TEST(TestJob, HandleShouldRunOn) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1", "level2"})
      ("node_source", "range_label")
    )
  );
  dynamic d = dynamic::object("owner", "owner");
  Job j(c, "job_name", d);

  Node instance_node("instance_node", 0, true);  // Can run on the root node
  EXPECT_EQ(Job::ShouldRun::Yes, j.shouldRunOn(instance_node));

  Node child("foo", 1, true);
  EXPECT_EQ(Job::ShouldRun::Yes, j.shouldRunOn(child));

  Node parent("foo_parent", 0, false);
  Node child2("foo", 1, true, &parent);
  EXPECT_EQ(Job::ShouldRun::NoDisabled, j.shouldRunOn(parent));
  EXPECT_EQ(Job::ShouldRun::NoDisabled, j.shouldRunOn(child2));

  d["filters"] = dynamic::object("level1", dynamic::object
      ("whitelist", { "missing_node" }));

  Job j2(c, "job_name", d);
  Node parent2("parent2", 1, true);
  Node child3("child3", 2, true, &parent2);
  EXPECT_EQ(Job::ShouldRun::NoAvoided, j2.shouldRunOn(parent2));
  EXPECT_EQ(Job::ShouldRun::NoAvoided, j2.shouldRunOn(child3));
}
