/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/nodes/test/utils.h"
#include "bistro/bistro/statuses/SQLiteTaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/utils/TemporaryFile.h"

using namespace facebook::bistro;
using namespace std;
using namespace folly;
using namespace folly::test;

cpp2::RunningTask makeRT(const string& job, const string& node) {
  cpp2::RunningTask rt;
  rt.job = job;
  rt.node = node;
  return rt;
}

dynamic c = dynamic::object
  ("nodes", dynamic::object
    ("levels", dynamic::array("host", "db"))
    ("node_sources", dynamic::array(dynamic::object
      ("source", "manual")
      ("prefs", dynamic::object("host", dynamic::array("db1", "db2", "db3")))
    ))
  )
  ("resources", dynamic::object)
  ("backoff", dynamic::array("fail"))
;

TEST(TestTaskStatuses, HandleTaskStore) {
  CaptureFD stderr(2);  // Don't capture stdout, since gtest errors go there

  Config config_job1(c);
  config_job1.addJob(
    std::make_shared<Job>(
        config_job1,
        "job1",
        dynamic::object("owner", "owner")),
    nullptr);
  Config config_no_job(c);

  auto nodes_ptr = make_shared<Nodes>();
  NodesLoader::_fetchNodesImpl(config_job1, nodes_ptr.get());
  const auto& job = config_job1.jobs["job1"];
  const auto& db1 = getNodeVerySlow(*nodes_ptr, "db1");
  const auto& db2 = getNodeVerySlow(*nodes_ptr, "db2");
  const auto& db3 = getNodeVerySlow(*nodes_ptr, "db3");

  TemporaryDir db_dir;
  auto task_statuses = make_shared<TaskStatuses>(
    make_shared<SQLiteTaskStore>(db_dir.getPath(), "test")
  );
  task_statuses->updateForConfig(config_job1);

  task_statuses->updateStatus(makeRT("job1", "db1"), TaskStatus::running());
  task_statuses->updateStatus(makeRT("job1", "db2"), TaskStatus::running());
  task_statuses->updateStatus(makeRT("job1", "db3"), TaskStatus::running());

  // A drive-by test of overwriteable statuses --
  // 1) This update will be overwritten by "done".
  auto status = TaskStatus::errorBackoff("ohnoes");
  status.markOverwriteable();
  task_statuses->updateStatus(makeRT("job1", "db1"), std::move(status));
  EXPECT_TRUE(
    task_statuses->copySnapshot().getPtr(job->id(), db1->id())->isInBackoff(0)
  );
  // 2) This replaces the overwriteable update.
  task_statuses->updateStatus(makeRT("job1", "db1"), TaskStatus::done());
  EXPECT_TRUE(
    task_statuses->copySnapshot().getPtr(job->id(), db1->id())->isDone()
  );
  // 3) If you were to add another overwriteable status, the test would abort.
  // 4) A second application-generated status is ignored, and prints an error.
  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
  task_statuses->updateStatus(makeRT("job1", "db1"), TaskStatus::failed());
  EXPECT_TRUE(
    task_statuses->copySnapshot().getPtr(job->id(), db1->id())->isDone()
  );
  EXPECT_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());

  auto rt = makeRT("job1", "db2");
  rt.nextBackoffDuration.noMoreBackoffs = true;
  task_statuses->updateStatus(rt, TaskStatus::errorBackoff(""));

  // job1:db3 is still running, let's delete the job, and make sure that
  // the running task is still tracked.
  task_statuses->updateForConfig(config_no_job);
  auto status_snapshot = task_statuses->copySnapshot();
  EXPECT_TRUE(status_snapshot.isJobLoaded(job->id()));
  EXPECT_TRUE(status_snapshot.getPtr(job->id(), db3->id())->isRunning());

  // End the last running task so that the job's statuses can be unloaded.
  task_statuses->updateStatus(
    makeRT("job1", "db3"),
    TaskStatus::incomplete(std::make_shared<dynamic>(dynamic::object()))
  );
  task_statuses->updateForConfig(config_no_job);
  status_snapshot = task_statuses->copySnapshot();
  EXPECT_FALSE(status_snapshot.isJobLoaded(job->id()));
  // Can't call getPtr here, or the test will crash.

  // Re-adding the job should load done/failed tasks.
  task_statuses->updateForConfig(config_job1);
  status_snapshot = task_statuses->copySnapshot();
  EXPECT_TRUE(status_snapshot.isJobLoaded(job->id()));
  EXPECT_TRUE(status_snapshot.getPtr(job->id(), db1->id())->isDone());
  EXPECT_TRUE(status_snapshot.getPtr(job->id(), db2->id())->isFailed());
  EXPECT_EQ(nullptr, status_snapshot.getPtr(job->id(), db3->id()));

  // simulate a restart to test if we write and read done/failed tasks properly
  task_statuses.reset(new TaskStatuses(
    make_shared<SQLiteTaskStore>(db_dir.getPath(), "test")
  ));

  EXPECT_FALSE(task_statuses->copySnapshot().isJobLoaded(job->id()));

  task_statuses->updateForConfig(config_job1);
  status_snapshot = task_statuses->copySnapshot();
  EXPECT_TRUE(status_snapshot.isJobLoaded(job->id()));
  EXPECT_TRUE(status_snapshot.getPtr(job->id(), db1->id())->isDone());
  EXPECT_TRUE(status_snapshot.getPtr(job->id(), db2->id())->isFailed());
  EXPECT_EQ(nullptr, status_snapshot.getPtr(job->id(), db3->id()));

  EXPECT_NO_PCRE_MATCH(glogErrOrWarnPattern(), stderr.readIncremental());
}
