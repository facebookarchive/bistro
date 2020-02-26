/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Conv.h>
#include <folly/experimental/TestUtil.h>
#include <folly/json.h>
#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/parsing_common.h"

using namespace facebook::bistro;
using folly::dynamic;

void checkValueParseError(
    const folly::dynamic& d,
    const Job& j,
    const std::vector<std::string>& key_path,
    const std::string& expected_error) {

  dynamic expected = dynamic::object();
  dynamic* expected_cur = &expected;
  const dynamic* actual_cur = &j.errors();
  const dynamic* value_cur = &d;
  for (const auto& key : key_path) {
    actual_cur = &(*actual_cur)["nested"][key];
    expected_cur = &expected_cur->setDefault("nested").setDefault(key);
    value_cur = &(*value_cur)[key];
  }
  (*expected_cur)["error"] = (*actual_cur)["error"];
  (*expected_cur)["value"] = *value_cur;
  EXPECT_EQ(expected, j.errors());
  EXPECT_EQ(expected_error, (*actual_cur)["error"].asString());
}

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
    (kNodes, dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(
        dynamic::object("source", "range_label")
      ))
    );
  Config c(cd1);
  dynamic jd = dynamic::object
    (kEnabled, false)
    ("owner", "owner")
    ("priority", 0.5)
    ("resources", dynamic::object("concurrency", 2))
    ("config", dynamic::object("key", "val"))
    ("filters", dynamic::object
      ("level2", dynamic::object
        ("whitelist_regex", "abc")
      )
    )
    ("depends_on", dynamic::array("job1", "job2"))
    ("version_id", 123456789)
  ;

  Job j(c, "foo_job", jd);
  EXPECT_EQ(0, j.id());
  EXPECT_EQ("foo_job", j.name());
  EXPECT_TRUE(j.isValid());
  EXPECT_FALSE(j.canRun());
  EXPECT_EQ(jd["config"], j.config());
  EXPECT_EQ(ResourceVector{2}, j.resources());
  // Starting from 1 because we had just inserted foo_job into JobNameTable
  EXPECT_EQ(
    j.dependencies(),
    std::vector<Job::ID>({Job::ID(1), Job::ID(2)})
  );

  // If a valid job is enabled, it can run.
  {
    auto jd2 = jd;
    jd2[kEnabled] = true;
    Job j2(c, "foo_job", jd2);
    EXPECT_TRUE(j2.isValid());
    EXPECT_TRUE(j2.canRun());
  }

  Job j2(c, "foo_job", jd);
  auto jd_new = j.toDynamic(c);
  EXPECT_EQ(jd_new, j2.toDynamic(c));

  EXPECT_EQ(123456789, jd_new.at("version_id").asInt());

  // Check the "kill orphans" option -- first, inherited, then overridden.
  EXPECT_FALSE(j.killOrphanTasksAfter().has_value());
  {
    cd1[kKillOrphanTasksAfterSec] = false;
    Job jh1(Config(cd1), "bar_job", jd);
    EXPECT_FALSE(jh1.killOrphanTasksAfter().has_value());
  }
  {
    cd1[kKillOrphanTasksAfterSec] = true;
    Job jh1(Config(cd1), "bar_job", jd);
    EXPECT_EQ(std::chrono::seconds(0), jh1.killOrphanTasksAfter().value());
  }
  {
    jd[kKillOrphanTasksAfterSec] = 0.37;
    Job jh1(Config(cd1), "bar_job", jd);
    EXPECT_EQ(
      std::chrono::milliseconds(370), jh1.killOrphanTasksAfter().value()
    );
  }

  cpp2::TaskSubprocessOptions task_opts;
  EXPECT_EQ(task_opts, j.taskSubprocessOptions());

  // Check non-default task options via Config
  cd1[kTaskSubprocess] = dynamic::object
    (kPollMs, 111)
    (kMaxLogLinesPerPollInterval, 222)
    (kParentDeathSignal, 333)
    (kProcessGroupLeader, true)
    (kUseCanaryPipe, false);
  task_opts.pollMs = 111;
  task_opts.maxLogLinesPerPollInterval = 222;
  task_opts.parentDeathSignal = 333;
  task_opts.processGroupLeader = true;
  task_opts.useCanaryPipe = false;
  EXPECT_EQ(task_opts, Job(Config(cd1), "j", jd).taskSubprocessOptions());

  // Further override task options via Job
  jd[kTaskSubprocess] = dynamic::object
    (kMaxLogLinesPerPollInterval, 444)
    (kUseCanaryPipe, true);
  task_opts.maxLogLinesPerPollInterval = 444;
  task_opts.useCanaryPipe = true;
  EXPECT_EQ(task_opts, Job(Config(cd1), "j", jd).taskSubprocessOptions());

  // Check task options' toDynamic
  EXPECT_EQ(
    dynamic(dynamic::object
      (kPollMs, 111)
      (kMaxLogLinesPerPollInterval, 444)
      (kParentDeathSignal, 333)
      (kProcessGroupLeader, true)
      (kUseCanaryPipe, true)
      (kCGroups, dynamic::object
        (kRoot, "")
        (kSlice, "")
        (kSubsystems, dynamic::array())
        (kKillWithoutFreezer, false)
      )
    ),
    Job(Config(cd1), "j", jd).toDynamic(c).at(kTaskSubprocess)
  );

  cpp2::KillRequest kill_req;
  EXPECT_EQ(kill_req, j.killRequest());

  // Non-default kill request via Config
  cd1[kKillSubprocess] = dynamic::object(kMethod, kKill)(kKillWaitMs, 987);
  kill_req.method = cpp2::KillMethod::KILL;
  kill_req.killWaitMs = 987;
  EXPECT_EQ(kill_req, Job(Config(cd1), "j", jd).killRequest());

  // Further override kill request via Job
  jd[kKillSubprocess] = dynamic::object(kMethod, kTermWaitKill);
  kill_req.method = cpp2::KillMethod::TERM_WAIT_KILL;
  EXPECT_EQ(kill_req, Job(Config(cd1), "j", jd).killRequest());

  // Check kill request's toDynamic
  EXPECT_EQ(
    dynamic(dynamic::object(kMethod, kTermWaitKill)(kKillWaitMs, 987)),
    Job(Config(cd1), "j", jd).toDynamic(c).at(kKillSubprocess)
  );

  // Check default, non-default, and toDynamic for non-default "command".
  EXPECT_TRUE(j.command().empty());
  std::vector<std::string> command{"a", "b", "c"};
  auto d_command = dynamic(command.begin(), command.end());
  jd[kCommand] = d_command;
  {
    auto j_new = Job(Config(cd1), "j", jd);
    EXPECT_EQ(command, j_new.command());
    EXPECT_EQ(d_command, j_new.toDynamic(c).at(kCommand));
  }

  // Check default, invalid, and valid level_for_host_placement.
  EXPECT_EQ(StringTable::NotFound, j.levelForHostPlacement());
  {
    jd["level_for_host_placement"] = "not a level";
    Job jh1(c, "bar_job", jd);
    EXPECT_FALSE(jh1.isValid());
    checkValueParseError(
      jd, jh1, {"level_for_host_placement"}, "Unknown identifier"
    );
  }
  // Ok to have level_for_host_placement and host_placement (host wins).
  EXPECT_EQ(StringTable::NotFound, j.levelForHostPlacement());
  {
    jd["level_for_host_placement"] = "level2";
    jd["host_placement"] = "some.host";
    Job jh1(c, "bar_job", jd);
    EXPECT_TRUE(jh1.isValid());
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
  cd1[kLevelForTasks] = "level2";  // Make the default explicit
  {
    Config ct(cd1);
    EXPECT_EQ(2, Job(ct, "foo_job", jd).levelForTasks());
  }
  cd1[kLevelForTasks] = "level1";  // Change the Config and the Job changes
  {
    Config ct(cd1);
    EXPECT_EQ(1, Job(ct, "foo_job", jd).levelForTasks());
    jd[kLevelForTasks] = "instance";  // But the Job has the final say
    EXPECT_EQ(0, Job(ct, "foo_job", jd).levelForTasks());
    // Error on invalid level
    jd[kLevelForTasks] = "invalid_level";
    Job je(ct, "foo_job", jd);
    EXPECT_FALSE(je.isValid());
    EXPECT_FALSE(je.canRun());
    checkValueParseError(jd, je, {kLevelForTasks.str()}, "Unknown identifier");
  }
}

TEST(TestJob, HandleError) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    (kNodes, dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(
        dynamic::object("source", "range_label")
      ))
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
  EXPECT_FALSE(j.canRun());
  checkValueParseError(
    d, j, {"filters", "invalid_level"}, "Unknown identifier"
  );
}

TEST(TestJob, SaveLoadCyclesWithManyErrors) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    (kNodes, dynamic::object("levels", dynamic::array()))
    // We used to have a bug where the Config's subsystems got **appended**
    // to the Job's subsystems.
    (kTaskSubprocess, dynamic::object
      (kCGroups, dynamic::object(kSubsystems, dynamic::array("config_subsys")))
    )
  );
  dynamic subsys_d = dynamic::array("subsys1", "subsys2");
  dynamic d = dynamic::object
    // Error 1: Missing owner
    ("priority", 1.3)  // ok
    ("depends_on", dynamic::array(
      "job1", "job2", dynamic::array("not a string")  // first two ok
    ))
    (kTaskSubprocess, dynamic::object
      (kCGroups, dynamic::object(kSubsystems, subsys_d))  // ok
    )
    ("version_id", "not an int")
  ;
  Job j(c, "foo_job", d);
  EXPECT_FALSE(j.isValid());
  EXPECT_FALSE(j.canRun());

  // Check we got the expected errors
  auto errors = j.errors();
  auto dep_job_err =
    errors.at("nested").at("depends_on").at("nested").at("2").at("error");
  EXPECT_PCRE_MATCH("TypeError: .*", dep_job_err.getString());
  auto version_err = errors.at("nested").at("version_id").at("error");
  EXPECT_PCRE_MATCH(
    ".*Invalid leading character.*: .not an int.",
    version_err.getString()
  );
  dynamic expected_errors = dynamic::object
    ("key_errors", dynamic::object
      ("owner", "Couldn't find key owner in dynamic object")
    )
    ("value", d)
    ("nested", dynamic::object
      ("depends_on", dynamic::object
        ("nested", dynamic::object
          ("2", dynamic::object
            ("error", dep_job_err)("value", dynamic::array("not a string"))
          )
        )
      )
      ("version_id", dynamic::object
        ("error", version_err)("value", "not an int")
      )
    )
  ;
  EXPECT_EQ(expected_errors, errors);

  // Conversion to dynamic is as expected. This simulates a save-load cycle.
  auto new_d = j.toDynamic(c);
  // A few fields are made concrete, but aren't good to hardcoding in the test.
  auto backoff_d = new_d.at("backoff");
  EXPECT_TRUE(backoff_d.isArray());
  auto kill_subprocess_d = new_d.at(kKillSubprocess);
  EXPECT_TRUE(kill_subprocess_d.isObject());
  auto task_subprocess_d = new_d.at(kTaskSubprocess);
  EXPECT_EQ(subsys_d, task_subprocess_d.at(kCGroups).at(kSubsystems));
  EXPECT_EQ(dynamic(dynamic::object
    // The "ok" values from above are preserved:
    ("priority", 1.3)
    ("depends_on", dynamic::array("job1", "job2"))
    // These fields are made concrete:
    ("enabled", true)  // But this job cannot run due to errors.
    ("config", dynamic::object())
    ("backoff", backoff_d)
    ("kill_subprocess", kill_subprocess_d)
    ("task_subprocess", task_subprocess_d)  // subsys_d was preserved
    ("level_for_tasks", "instance")
    ("kill_orphan_tasks_after_sec", false)
    // It's lame to export "", but this prevents indefinite nesting of
    // errors on save-load.  Might reconsider this behavior in the future.
    ("owner", "")
    ("errors", expected_errors)
  ), new_d);

  // Let's try a "save-load-modify-save-load" now.
  new_d["level_for_host_placement"] = "an invalid level";
  new_d["owner"] = "a new owner";

  // We introduced a new error, so the old errors will get nested. The new
  // owner value will not be lost, either.
  Job j2(c, "foo_job", new_d);
  auto level_err =
    j2.errors().at("nested").at("level_for_host_placement").at("error");
  EXPECT_EQ("Unknown identifier", level_err.getString());
  auto nested_err = j2.errors().at("nested").at("errors").at("error");
  EXPECT_EQ("Pre-existing errors", nested_err.getString());
  dynamic expected_d = new_d;
  expected_d.at("errors") = dynamic::object
    ("nested", dynamic::object
      ("level_for_host_placement", dynamic::object
        ("value", "an invalid level")("error", level_err)
      )
      ("errors", dynamic::object
        ("value", expected_d.at("errors"))("error", nested_err)
      )
    )
  ;
  expected_d.erase("level_for_host_placement");
  EXPECT_EQ(expected_d.at("errors"), j2.errors());
  EXPECT_EQ(expected_d, j2.toDynamic(c));
}

TEST(TestJob, HandleShouldRunOn) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    (kNodes, dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(
        dynamic::object("source", "range_label")
      ))
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

  d["filters"] = dynamic::object
    ("level1", dynamic::object("whitelist", dynamic::array("missing_node")));

  Job j2(c, "job_name", d);
  Node parent2("parent2", 1, true);
  Node child3("child3", 2, true, &parent2);
  EXPECT_EQ(Job::ShouldRun::NoAvoided, j2.shouldRunOn(parent2));
  EXPECT_EQ(Job::ShouldRun::NoAvoided, j2.shouldRunOn(child3));
}
