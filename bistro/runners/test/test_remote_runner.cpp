/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Random.h>
#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/runners/RemoteWorkerRunner.h"
#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/WorkerSetID.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/server/HTTPMonitor.h"
#include "bistro/bistro/test/utils.h"
#include "bistro/bistro/worker/test/FakeBistroWorkerThread.h"

DECLARE_int32(CAUTION_startup_wait_for_workers);
DECLARE_int32(incremental_sleep_ms);

using namespace facebook::bistro;
using folly::dynamic;

struct TestRemoteRunner : public ::testing::Test {
  TestRemoteRunner() {
    // Don't block starting tasks due to 'initial wait'.
    FLAGS_CAUTION_startup_wait_for_workers = 0;
    // Minimize delay between RemoteWorkerRunner's updateState calls.
    FLAGS_worker_check_interval = 1;
    // These tests are heavily threaded, and death tests don't work otherwise.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }
};

cpp2::BistroInstanceID randInstanceID() {
  cpp2::BistroInstanceID id;
  id.startTime = folly::Random::rand64();
  id.rand = folly::Random::rand64();
  return id;
}

cpp2::WorkerSetID addToWorkerSetID(
    cpp2::WorkerSetID wsid,
    const FakeBistroWorkerThread& wt) {
  addWorkerIDToHash(&wsid.hash, wt.getBistroWorker().id);
  ++wsid.version;
  return wsid;
}

// Wait for RemoteWorkerRunner to realize that the initial wait is over.
void waitToExitInitialWait(const RemoteWorkerRunner& runner) {
  while (runner.inInitialWaitForUnitTest()) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

cpp2::WorkerSetID addFakeWorker(
    RemoteWorkerRunner* runner,
    std::shared_ptr<const Config> config,
    const FakeBistroWorkerThread& wt,
    cpp2::WorkerSetID wsid,
    int64_t cur_time = time(nullptr)) {  // The current set of workers

  folly::test::CaptureFD stderr(2, printString);

  // Connect the worker to the Runner.
  auto res = runner->processWorkerHeartbeat(
    wt.getBistroWorker(), cpp2::WorkerSetID(),  // The 'old' scheduler's set ID
    // Cannot use UNIT_TEST_TIME here or elsewhere, since RemoteWorkerRunner
    // has a background thread that calls updateState with the system time.
    RemoteWorkerUpdate()
  );
  wsid = addToWorkerSetID(wsid, wt);
  EXPECT_EQ(wsid, res.workerSetID) << apache::thrift::debugString(wsid)
    << " != " << apache::thrift::debugString(res.workerSetID);

  // Wait the Runner to get running tasks & to request a healthcheck, in
  // either order.
  auto kGotRunningTasks = folly::to<std::string>(
    ".* Recorded 0 new running tasks for ", wt.shard(), " .*"
  );
  auto kSentHealthcheck = ".* Sent healthchecks to 1 workers .*";
  waitForRegexOnFd(&stderr, folly::to<std::string>(
    "(", kGotRunningTasks, kSentHealthcheck, "|",
         kSentHealthcheck, kGotRunningTasks, ")"
  ));

  // Fake a reply to the healthcheck -- does not run RemoteWorker::updateState.
  cpp2::RunningTask rt;
  rt.job = kHealthcheckTaskJob;
  rt.workerShard = wt.shard();
  rt.invocationID.startTime = cur_time;  // For "time since healthcheck"
  runner->remoteUpdateStatus(
    rt,
    TaskStatus::done(),
    runner->getSchedulerID(),
    wt.getBistroWorker().id
  );

  // The minimum worker_check_interval is 1 second, so send heartbeats to
  // speed up the worker becoming healthy.  We need two -- the first sets
  // RemoteWorker::workerSetID_, the second triggers 'consensus permits'.
  for (int i = 0; i < 2; ++i) {
    runner->processWorkerHeartbeat(
      wt.getBistroWorker(), res.workerSetID,  // As if the worker echoed the ID
      RemoteWorkerUpdate()
    );
  }

  waitForRegexOnFd(&stderr, folly::to<std::string>(
    ".* ", wt.shard(), " became healthy.*"
  ));
  stderr.release();

  // Register the new worker's resources
  runner->updateConfig(config);

  return wsid;
}

const int kDefaultBackoff = 5;
const dynamic kJob = dynamic::object
  ("enabled", true)
  ("owner", "owner")
  // LostTasksHaveAMinimumBackoff below needs to know the default backoff.
  // It also relies on there being exactly 1 backoff before failure.
  ("backoff", dynamic::array(kDefaultBackoff, "fail"));

TEST_F(TestRemoteRunner, HandleResources) {
  // Create the worker thread first so that the worker exits *after* the
  // runner does (this avoids harmless error messages about the runner being
  // unable to talk to the worker).
  const auto worker_id = randInstanceID();
  FakeBistroWorkerThread worker(
    "test_worker",
    [&worker_id](cpp2::BistroWorker* w) { w->id = worker_id; }
  );

  const auto kConfig = std::make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", dynamic::array()))
    ("resources", dynamic::object
      ("worker", dynamic::object
        ("concurrency", dynamic::object
          ("default", 1)
          ("limit", 1)
        )
      )
    ));

  auto task_statuses =
    std::make_shared<TaskStatuses>(std::make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, std::shared_ptr<Monitor>());

  auto job = std::make_shared<Job>(*kConfig, "foo_job", kJob);
  Node node1("test_node1");
  Node node2("test_node2");

  // Cannot run a task without workers
  runner.updateConfig(kConfig);
  auto res = runner.runTask(
    *kConfig,
    job,
    node1,
    nullptr,  // no previous status
    [](const cpp2::RunningTask&, TaskStatus&&) { FAIL() << "Never runs"; }
  );
  ASSERT_EQ(TaskRunnerResponse::DoNotRunMoreTasks, res);

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  addFakeWorker(&runner, kConfig, worker, wsid);
  waitToExitInitialWait(runner);

  // The first task will consume all of `test_worker`'s `concurrency`.
  {
    folly::Promise<folly::Unit> cob_ran;
    ASSERT_EQ(TaskRunnerResponse::RanTask, runner.runTask(
      *kConfig,
      job,
      node1,
      nullptr,  // no previous status
      [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
        ASSERT_EQ("foo_job", rt.job);
        ASSERT_EQ("test_node1", rt.node);
        ASSERT_TRUE(status.isRunning());
        cob_ran.setValue();
      }
    ));
    cob_ran.getFuture().get();  // Ensure the cob runs.
  }

  // Can't run another task because of the worker resource limit.
  res = runner.runTask(
    *kConfig,
    job,
    node2,
    nullptr,  // no previous status
    [](const cpp2::RunningTask&, TaskStatus&&) { FAIL() << "Never runs"; }
  );
  ASSERT_EQ(TaskRunnerResponse::DidNotRunTask, res);
}

struct FakeWorker {
  explicit FakeWorker(std::string shard)
    : workerID_(randInstanceID()),
      worker_(
        shard,
        [&](cpp2::BistroWorker* w) { w->id = workerID_; },
        // Each FakeBistroWorker calls this just before a task's "done" cob.
        [&](const cpp2::RunningTask& rt, const cpp2::TaskSubprocessOptions&) {
          if (rt.job == "foo_job") {
            CHECK(taskCobCob_.has_value());
            taskCobCob_.value()(rt);
          }
        }
      ) {}

  // **MUST** be called before running any tasks.
  void connect(
      int wsid_version,
      std::shared_ptr<const Config> config,
      std::shared_ptr<RemoteWorkerRunner> runner,
      int64_t cur_time) {

    taskCobCob_ = [this, runner](const cpp2::RunningTask& rt) {
      runner->remoteUpdateStatus(
        rt,
        // Fake receiving a reply from the remote worker, **after** the test
        // permits this.
        workerMayReturnStatus_.getFuture().get(),
        runner->getSchedulerID(),
        worker_.getBistroWorker().id
      );
      // Allows us to wait until a worker successfully marks the task done.
      workerReturnedStatus_.setValue();
    };

    wsid_.schedulerID = runner->getSchedulerID();
    wsid_.version = wsid_version;
    wsid_ = addFakeWorker(runner.get(), config, worker_, wsid_, cur_time);
  }

  cpp2::BistroInstanceID workerID_;
  // Lazily initialized to allow adding custom logic into the cob that is
  // called when a worker receives a task.
  folly::Optional<std::function<void(const cpp2::RunningTask&)>> taskCobCob_;
  FakeBistroWorkerThread worker_;
  folly::Promise<TaskStatus> workerMayReturnStatus_;
  folly::Promise<folly::Unit> workerReturnedStatus_;
  cpp2::WorkerSetID wsid_;
};

//
// Shared code between a couple of tests that start a task, and then let the
// worker running it be lost.
//

// constexpr segfaults clang, static constexpr class members are hard
const char* kOneTaskJobName = "foo_job";
const char* kOneTaskNodeName = "test_node";

struct TestRemoteRunnerWithOneTask : TestRemoteRunner {
  TestRemoteRunnerWithOneTask()
    : TestRemoteRunner(),
      // Create the worker threads first so that the workers exit *after*
      // the runner does (this avoids harmless error messages about the
      // runner being unable to talk to the worker).
      workers_([]() {
        std::vector<std::unique_ptr<FakeWorker>> v;
        v.emplace_back(std::make_unique<FakeWorker>("test_worker_1"));
        v.emplace_back(std::make_unique<FakeWorker>("test_worker_2"));
        return v;
      }()),
      config_([]() {
        auto cfg = std::make_shared<Config>(dynamic::object
          ("enabled", true)
          ("nodes", dynamic::object("levels", dynamic::array()))
          ("resources", dynamic::object));
        cfg->addJob(
            std::make_shared<Job>(*cfg, kOneTaskJobName, kJob),
            /*prev_config =*/ nullptr);
        return cfg;
      }()),
      job_(config_->jobs.at(kOneTaskJobName)),
      node_(kOneTaskNodeName),
      taskStatuses_(
        std::make_shared<TaskStatuses>(std::make_shared<NoOpTaskStore>())
      ),
      runner_(std::make_shared<RemoteWorkerRunner>(
        taskStatuses_, std::shared_ptr<Monitor>()
      )),
      curTime_(time(nullptr)) {

    // Otherwise getPtr() will fail with "job not loaded".
    taskStatuses_->updateForConfig(*config_);
  }

  void connectWorker(int idx, int wsid_version = 0) {
    workers_[idx]->connect(wsid_version, config_, runner_, curTime_);
  }

  void runTask() {
    // The rest of the tests in this file use a promise with runTask to
    // check that the cob runs.  This seems to imply a wait /
    // synchronization, but in actuality, I think this is supposed to run
    // synchronously, in the same thread, so assert that instead.
    bool cob_ran = false;
    auto outer_thread = std::this_thread::get_id();
    auto status_snapshot = taskStatuses_->copySnapshot();
    ASSERT_EQ(TaskRunnerResponse::RanTask, runner_->runTask(
      *config_,
      job_,
      node_,
      // The previous status, if any.
      status_snapshot.getPtr(job_->id(), node_.id()),
      [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
        ASSERT_EQ(kOneTaskJobName, rt.job);
        ASSERT_EQ(kOneTaskNodeName, rt.node);
        ASSERT_TRUE(status.isRunning());
        ASSERT_EQ(outer_thread, std::this_thread::get_id());
        taskStatuses_->updateStatus(
          job_->id(), node_.id(), rt, std::move(status)
        );
        cob_ran = true;
      }
    ));
    ASSERT_TRUE(cob_ran);
  }

  TaskStatus getTaskStatus() {
    auto snapshot = taskStatuses_->copySnapshot();
    auto job_id = Job::JobNameTable->lookup(kOneTaskJobName);
    auto node_id = Node::NodeNameTable->lookup(kOneTaskNodeName);
    CHECK_NE(StringTable::NotFound, job_id);
    CHECK_NE(StringTable::NotFound, node_id);
    if (auto* status = snapshot.getPtr(
      static_cast<Job::ID>(job_id), static_cast<Node::ID>(node_id)
    )) {
      LOG(INFO) << "Current status in TaskStatuses: " << status->toJson();
      return *status;
    }
    LOG(FATAL) << "Got nullptr instead of a status";
  }

  void missHealthcheckForWorker(
      int idx,
      std::function<void()> between_update_and_apply_cob = []() {}) {

    curTime_ += 10000000;
    auto& w = workers_.at(idx);
    runner_->processWorkerHeartbeat(
      w->worker_.getBistroWorker(),
      w->wsid_,
      RemoteWorkerUpdate(RemoteWorkerUpdate::UNIT_TEST_TIME, curTime_),
      between_update_and_apply_cob
    );
  }

  std::vector<std::unique_ptr<FakeWorker>> workers_;

  const std::shared_ptr<const Config> config_;
  std::shared_ptr<const Job> job_;
  Node node_;

  std::shared_ptr<TaskStatuses> taskStatuses_;
  // This is shared to let FakeWorker's callbacks extend its lifetime.
  std::shared_ptr<RemoteWorkerRunner> runner_;

  time_t curTime_;
};

TEST_F(TestRemoteRunnerWithOneTask, TaskExitedRacesTaskLost) {
  connectWorker(0);
  waitToExitInitialWait(*runner_);
  runTask();
  // The first missed healthcheck makes the worker unhealthy.
  missHealthcheckForWorker(0);
  // The second missed healthcheck makes the worker and the task lost.
  {
    folly::test::CaptureFD stderr(2, printString);
    missHealthcheckForWorker(
      0,
      // This happens after the task is written into RemoteWorkerUpdate as
      // lost, but before the update is actually applied.
      [&]() {
        // Have the worker return the status just after the "process
        // heartbeat" update is computed -- this minimizes the chances that
        // the background thread `applyUpdate` will break the test.
        workers_[0]->workerMayReturnStatus_.setValue(TaskStatus::done());
        // Simulate a race with a remote worker by waiting until this task's
        // status is modified by remoteUpdateStatus above.
        workers_[0]->workerReturnedStatus_.getFuture().get();
      }
    );
    // This part of the test could, in a very unlikely scenario, fail
    // because the RemoteWorkerRunner background thread calls updateState &
    // applyUpdate while we are waiting for `worker_returned_status` above.
    // I choose not to worry about it :)
    EXPECT_PCRE_MATCH(
      ".* the worker won a race against marking the task as lost: .*",
      stderr.readIncremental()
    );
  }
}

TEST_F(TestRemoteRunnerWithOneTask, DeathDueToTaskThatTookTooLongToKill) {
  connectWorker(0);
  waitToExitInitialWait(*runner_);
  runTask();
  // First, make the worker unhealthy, then -- lost.
  missHealthcheckForWorker(0);
  missHealthcheckForWorker(0);
  // Two version bumps: w1 joined -> 1, w1 died -> 2
  connectWorker(1, /*worker set version*/ 2);
  runTask();
  // This tests one of several safeguards in Bistro, which ensure that it
  // crashes when it detects that it has started two copies of one task.
  //
  // The reason this particular sequence of events triggers a CHECK is as
  // follows:
  //
  //  - The lost task still exists on the worker, which is trying to
  //    kill it off.
  //
  //  - The scheduler starts a new task on a different worker, violating
  //    one of its cardinal promises (presumably because its "safety" wait
  //    was too short, or the worker machine's kernel had serious issues
  //    with killing a task).
  //
  //  - The lost task's exit status finally arrives. Since each
  //    RemoteWorker tracks its own runningTasks_, it concludes that
  //    this is just updating the "lost" status for the task with
  //    a proper return value -- a supported operation. However,
  //    TaskStatuses has a global view of runningTasks_, and so
  //    TaskStatusSnapshot::updateStatus hits this invariant violation.
  //
  //    It would not be great to make this a non-CHECK for a variety of
  //    reasons, although if there is a real, practical case where
  //    converting the CHECK to an ERROR would be desirable, speak up.
  EXPECT_DEATH(
    {
      workers_[0]->workerMayReturnStatus_.setValue(TaskStatus::done());
      // This is triggered **after** the call to remoteUpdateStatus(), which
      // is where the CHECK would have to happen.  We have to explicitly
      // wait for this, since gtest's "threadsafe" mode seems to skip
      // destructors, and just fast-exit immediately after it runs the
      // "death" code.
      workers_[0]->workerReturnedStatus_.getFuture().get();
    },
    // This message TaskStatusSnapshot::updateStatus asserts that the old,
    // lost worker sent "done" after a new task was already running -- in
    // other words, the scheduler must have screwed and started a second
    // copy of a task **too soon**, and therefore must crash to ensure that
    // this failure mode is surfaced.
    //
    // The one way that this can happen through no fault of the scheduler is
    // if it takes a very long time to kill tasks due to kernel issues
    // leaving them in unkillable D states.  However, this should be
    // sufficiently rare that it is still worth surfacing as a crash.  If
    // you are affected, one workaround is to increase the
    // --CAUTION_worker_suicide_backoff_safety_margin_sec value to the point
    // where the kernel definitely has enough time to kill such stale tasks.
    ".* Check failed: it->second\\.invocationID == rt.invocationID Cannot "
    "updateStatus since the invocation IDs don't match, new task "
    "RunningTask \\{.*: workerShard \\(string\\) = \"test_worker_1\",.*"
    ": workerShard \\(string\\) = \"test_worker_2\",.*\\} with status "
    // This JSON log can be serialized in either order, so match both.
    "("
      "\\{\"time\":[0-9]*,\"result_bits\":4\\}"
    "|"
      "\\{\"result_bits\":4,\"time\":[0-9]*\\}"
    ")"
  );
}

// When a worker is lost, its tasks **need** to be in backoff for long
// enough that we are very confident that they have been killed
// successfully.  Otherwise, we would start a second copy of a task while
// the first is still being terminated, which breaks Bistro's contract.
TEST_F(TestRemoteRunnerWithOneTask, LostTasksHaveAMinimumBackoff) {
  // See the lostRunningTasks() handler in RemoteWorkerRunner for the details
  const int32_t kExtendedBackoff =
    60 +  // CAUTION_worker_suicide_backoff_safety_margin_sec
    5 + 1;  // CAUTION_worker_suicide_task_kill_wait_ms
  ASSERT_GT(kExtendedBackoff, kDefaultBackoff);

  connectWorker(0);
  waitToExitInitialWait(*runner_);
  runTask();
  // First, make the worker unhealthy, then -- lost.
  missHealthcheckForWorker(0);
  missHealthcheckForWorker(0);
  // Check that once the task stops running, it has the expected lost status.
  // We haven't fulfilled `workerMayReturnStatus_` yet, so this can only
  // be the intermediate lost status.
  while (true) {
    auto status = getTaskStatus();
    if (!status.isRunning()) {
      EXPECT_FALSE(status.isDone());
      EXPECT_FALSE(status.isFailed());
      EXPECT_TRUE(status.isOverwriteable());
      EXPECT_PCRE_MATCH(
        "Remote worker lost \\(.*\\)",
        status.dataThreadUnsafe()->at("exception").getString()
      );
      EXPECT_EQ(
        "test_worker_1",
        status.dataThreadUnsafe()->at("worker_shard").getString()
      );
      EXPECT_EQ(
        kDefaultBackoff,
        status.dataThreadUnsafe()->at("__bistro_saved_backoff").getInt()
      );
      // The next backoff value is computed on the basis of this struct.
      EXPECT_FALSE(status.configuredBackoffDuration().noMoreBackoffs);
      EXPECT_EQ(kDefaultBackoff, status.configuredBackoffDuration().seconds);
      // But the effective backoff duration is longer.
      auto expiration_time = status.timestamp() + kExtendedBackoff;
      EXPECT_TRUE(status.isInBackoff(expiration_time - 1));
      EXPECT_FALSE(status.isInBackoff(expiration_time));
      break;
    }
  }
  // The second half of this test will restart the same task, to see what
  // happens when a task runs out of retries due to a worker being lost.
  // The job permits exactly 1 backoff, and we have already used it.  The
  // way to preserve the backoff without failing is `incompleteBackoff()`:
  workers_[0]->workerMayReturnStatus_.setValue(TaskStatus::incompleteBackoff(
    std::make_shared<dynamic>(dynamic::object())
  ));
  workers_[0]->workerReturnedStatus_.getFuture().get();

  // Two version bumps: w1 joined -> 1, w1 died -> 2
  connectWorker(1, /*worker set version*/ 2);
  runTask();
  // First, make the worker unhealthy, then -- lost.
  missHealthcheckForWorker(1);
  missHealthcheckForWorker(1);
  // Check that once the task stops running, it has the expected lost status.
  // This time around, the task fails due to being out of backoffs.
  while (true) {
    auto status = getTaskStatus();
    if (!status.isRunning()) {
      EXPECT_FALSE(status.isDone());
      EXPECT_TRUE(status.isFailed());
      EXPECT_TRUE(status.isOverwriteable());
      EXPECT_PCRE_MATCH(
        "Remote worker lost \\(.*\\)",
        status.dataThreadUnsafe()->at("exception").getString()
      );
      EXPECT_EQ(
        "test_worker_2",
        status.dataThreadUnsafe()->at("worker_shard").getString()
      );
      EXPECT_EQ(
        60,  // a magical constant from JobBackoffSettings::getNext :'-(
        status.dataThreadUnsafe()->at("__bistro_saved_backoff").getInt()
      );
      // The next backoff value is computed on the basis of this struct.
      EXPECT_TRUE(status.configuredBackoffDuration().noMoreBackoffs);
      // No backoff, no duration (even though `60` was saved).
      EXPECT_EQ(0, status.configuredBackoffDuration().seconds);
      {
        // The effective backoff duration is intact.
        auto expiration_time = status.timestamp() + kExtendedBackoff;
        EXPECT_TRUE(status.isInBackoff(expiration_time - 1));
        EXPECT_FALSE(status.isInBackoff(expiration_time));
      }

      // Now, let's try to forgive the status, to ensure that the extended
      // backoff is preserved, but the configured backoff is forgiven.
      taskStatuses_->forgiveJob(kOneTaskJobName);

      auto new_status = getTaskStatus();
      EXPECT_FALSE(new_status.isDone());
      EXPECT_FALSE(new_status.isFailed());
      EXPECT_TRUE(new_status.isOverwriteable());
      EXPECT_PCRE_MATCH(
        "Remote worker lost \\(.*\\)",
        new_status.dataThreadUnsafe()->at("exception").getString()
      );
      EXPECT_EQ(
        "test_worker_2",
        new_status.dataThreadUnsafe()->at("worker_shard").getString()
      );
      EXPECT_EQ(
        0,  // was reset by forgive()
        new_status.dataThreadUnsafe()->at("__bistro_saved_backoff").getInt()
      );
      // The next backoff value is computed on the basis of this struct.
      EXPECT_FALSE(new_status.configuredBackoffDuration().noMoreBackoffs);
      EXPECT_EQ(0, new_status.configuredBackoffDuration().seconds);
      {
        // The effective backoff duration is intact.
        auto expiration_time = new_status.timestamp() + kExtendedBackoff;
        EXPECT_TRUE(new_status.isInBackoff(expiration_time - 1));
        EXPECT_FALSE(new_status.isInBackoff(expiration_time));
      }

      break;
    }
  }
  // Fulfill our promises just to avoid BrokenPromise logspam.
  workers_[1]->workerMayReturnStatus_.setValue(TaskStatus::done());
  workers_[1]->workerReturnedStatus_.getFuture().get();
}

TEST_F(TestRemoteRunner, MapLogicalResourcesToCGroupPhysical) {
  // Create the worker first for the same reasons as in HandleResources.
  folly::Promise<folly::Unit> tso_cob_ran;
  const auto worker_id = randInstanceID();
  FakeBistroWorkerThread worker(
    "test_worker",
    [&worker_id](cpp2::BistroWorker* w) { w->id = worker_id; },
    [&](const cpp2::RunningTask& rt, const cpp2::TaskSubprocessOptions& tso) {
      if (rt.job == kHealthcheckTaskJob) {
        return;
      }
      EXPECT_EQ(6, tso.cgroupOptions.cpuShares);  // Tests rounding
      EXPECT_EQ(3072, tso.cgroupOptions.memoryLimitInBytes);
      tso_cob_ran.setValue();
    }
  );

  const auto kConfig = std::make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", dynamic::array()))
    ("resources", dynamic::object
      ("worker", dynamic::object
        ("my_ram_gb", dynamic::object("default", 3)("limit", 48))
        ("my_cpu_centicore", dynamic::object("default", 270)("limit", 2000))
      )
    )
    (kPhysicalResources, dynamic::object
      (kRamMB, dynamic::object
        (kLogicalResource, "my_ram_gb")
        (kMultiplyLogicalBy, 1024)
        (kEnforcement, kHard)
      )
      (kCPUCore, dynamic::object
        (kLogicalResource, "my_cpu_centicore")
         // Don't downscale CPU in practice! That loses resolution, whereas
         // cgroups cpu.shares actually works with *relative* values anyhow.
        (kMultiplyLogicalBy, 0.01)
        (kEnforcement, kSoft)
      )
    )
  );

  auto task_statuses =
    std::make_shared<TaskStatuses>(std::make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, std::shared_ptr<Monitor>());

  auto job = std::make_shared<Job>(*kConfig, "job", kJob);
  Node node("node");

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  addFakeWorker(&runner, kConfig, worker, wsid);

  waitToExitInitialWait(runner);
  folly::Promise<folly::Unit> status_cob_ran;
  ASSERT_EQ(TaskRunnerResponse::RanTask, runner.runTask(
    *kConfig,
    job,
    node,
    nullptr,  // no previous status
    [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
      ASSERT_EQ("job", rt.job);
      ASSERT_EQ("node", rt.node);
      ASSERT_TRUE(status.isRunning());
      status_cob_ran.setValue();
    }
  ));
  // Check that both our cobs got executed.
  status_cob_ran.getFuture().get();
  tso_cob_ran.getFuture().get();
}

// Worker => resource => amount
using ResourceMap = std::map<std::string, std::map<std::string, int>>;

// Checks that the specified jobs start tasks on the instance node using the
// specified workers, and that the specified amount of worker resources
// remains after each task start.
void checkTasksRunOnWorkersLeavingResources(
    std::shared_ptr<const Config> config,
    RemoteWorkerRunner* runner,
    std::vector<std::tuple<
      // job name (must be unique), job config, worker name, resources *after*
      std::string, std::string, folly::dynamic, ResourceMap
    >> jobs_on_workers_with_resources) {
  // Run a bunch of different jobs with different resources, and check results.
  for (auto& t : jobs_on_workers_with_resources) {
    folly::Promise<folly::Unit> cob_ran;
    ASSERT_EQ(TaskRunnerResponse::RanTask, runner->runTask(
      *config,
      std::make_shared<Job>(*config, std::get<0>(t), dynamic::object
        ("enabled", true)
        ("owner", "owner")
        ("resources", std::get<2>(t))
      ),
      Node("test_node"),
      nullptr,  // no previous status
      [&](const cpp2::RunningTask& rt, TaskStatus&& status) {
        ASSERT_EQ(std::get<0>(t), rt.job);
        ASSERT_EQ("test_node", rt.node);
        ASSERT_EQ(std::get<1>(t), rt.workerShard) << std::get<0>(t);
        ASSERT_TRUE(status.isRunning());
        cob_ran.setValue();
      }
    ));
    cob_ran.getFuture().get();  // Ensure the cob runs.
    // Check that the resources are what we expect
    auto worker_resources = runner->copyResourcesForUnitTest();
    for (const auto& p_wr : std::get<3>(t)) {
      const auto& r_it = worker_resources.find(p_wr.first);
      ASSERT_NE(worker_resources.end(), r_it);
      for (const auto& p_rv : p_wr.second) {
        auto rid = config->resourceNames.lookup(p_rv.first);
        ASSERT_EQ(p_rv.second, r_it->second.at(rid))
          << std::get<0>(t) << " " << p_wr.first << " " << p_rv.first;
      }
    }
  }
}

// Future: Refactor this to use the TestRemoteRunnerWithOneTask harness above.
TEST_F(TestRemoteRunner, TestBusiestSelector) {
  const auto kConfig = std::make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", dynamic::array()))
    ("resources", dynamic::object
      ("worker", dynamic::object
        ("cheap", dynamic::object
          ("default", 1)
          ("limit", 30)
          ("weight", 1)
        )
        ("expensive", dynamic::object
          ("default", 1)
          ("limit", 3)
          ("weight", 10)
        )
        ("unranked", dynamic::object
          ("default", 1)
          ("limit", 5)
        )
      )
    )
    ("remote_worker_selector", "busiest")
    (kWorkerResourceOverride, dynamic::object
      // The second worker is slightly smaller, so we'll fill it first.
      ("w2", dynamic::object("cheap", 25))
    )
  );

  // Like in HandleResources, make the workers before making the runner.
  const auto w1_id = randInstanceID();
  FakeBistroWorkerThread worker1("w1", [&w1_id](cpp2::BistroWorker* w) {
    w->id = w1_id;
  });
  const auto w2_id = randInstanceID();
  FakeBistroWorkerThread worker2("w2", [&w2_id](cpp2::BistroWorker* w) {
    w->id = w2_id;
  });

  auto task_statuses =
    std::make_shared<TaskStatuses>(std::make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, std::shared_ptr<Monitor>());

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  wsid = addFakeWorker(&runner, kConfig, worker1, wsid);
  // Fake that w1 knows about w2, for consensusPermitsBecomingHealthy.
  runner.processWorkerHeartbeat(
    worker1.getBistroWorker(), addToWorkerSetID(wsid, worker2),
    RemoteWorkerUpdate()
  );
  wsid = addFakeWorker(&runner, kConfig, worker2, wsid);

  waitToExitInitialWait(runner);
  checkTasksRunOnWorkersLeavingResources(
    kConfig,
    &runner,
    {
      // This fits on either worker, but w2 started slightly smaller.
      std::make_tuple(
        "j1", "w2", dynamic(dynamic::object),
        ResourceMap{
          {"w1", {{"cheap", 30}, {"expensive", 3}, {"unranked", 5}}},
          {"w2", {{"cheap", 24}, {"expensive", 2}, {"unranked", 4}}},
        }
      ),
      // j2 fits on either, so it picks w2 which has less weight.
      std::make_tuple(
        "j2", "w2", dynamic(dynamic::object("expensive", 0)),
        ResourceMap{
          {"w1", {{"cheap", 30}, {"expensive", 3}, {"unranked", 5}}},
          {"w2", {{"cheap", 23}, {"expensive", 2}, {"unranked", 3}}},
        }
      ),
      // j3 must run on w1 since it won't fit on w2 due to "unranked".
      std::make_tuple(
        "j3", "w1", dynamic(dynamic::object("expensive", 2)("unranked", 4)),
        ResourceMap{
          {"w1", {{"cheap", 29}, {"expensive", 1}, {"unranked", 1}}},
          {"w2", {{"cheap", 23}, {"expensive", 2}, {"unranked", 3}}},
        }
      ),
      // j4 through j7 all pick w1 since it continues to have less weight.
      std::make_tuple(
        "j4", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 28}}}}
      ),
      std::make_tuple(
        "j5", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 27}}}}
      ),
      std::make_tuple(
        "j6", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 26}}}}
      ),
      std::make_tuple(
        "j7", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{
          {"w1", {{"cheap", 25}, {"expensive", 1}, {"unranked", 1}}},
          {"w2", {{"cheap", 23}, {"expensive", 2}, {"unranked", 3}}},
        }
      ),
      // j8 only switches to w2 because there's not enough "unranked" on w1.
      std::make_tuple(
        "j8", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 2)),
        ResourceMap{
          {"w1", {{"cheap", 25}, {"expensive", 1}, {"unranked", 1}}},
          {"w2", {{"cheap", 22}, {"expensive", 2}, {"unranked", 1}}},
        }
      ),
      // Continue gnawing at w1, since it still has less weight.
      std::make_tuple(
        "j9", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 24}}}}
      ),
      std::make_tuple(
        "j10", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 23}}}}
      ),
      std::make_tuple(
        "j11", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 22}}}}
      ),
      std::make_tuple(
        "j12", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 21}}}}
      ),
      std::make_tuple(
        "j13", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{
          {"w1", {{"cheap", 20}, {"expensive", 1}, {"unranked", 1}}},
          {"w2", {{"cheap", 22}, {"expensive", 2}, {"unranked", 1}}},
        }
      ),
      // Bounce to w2 since w1 lacks sufficient "expensive"
      std::make_tuple(
        "j14", "w2", dynamic(dynamic::object("expensive", 2)("unranked", 0)),
        ResourceMap{
          {"w1", {{"cheap", 20}, {"expensive", 1}, {"unranked", 1}}},
          {"w2", {{"cheap", 21}, {"expensive", 0}, {"unranked", 1}}},
        }
      ),
      // Now w2 has less weight, so gnaw at it awhile.
      std::make_tuple(
        "j15", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w2", {{"cheap", 20}}}}
      ),
      std::make_tuple(
        "j16", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w2", {{"cheap", 19}}}}
      ),
      std::make_tuple(
        "j17", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w2", {{"cheap", 18}}}}
      ),
      std::make_tuple(
        "j18", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w2", {{"cheap", 17}}}}
      ),
      std::make_tuple(
        "j19", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w2", {{"cheap", 16}}}}
      ),
      // One last switch to w1, consuming the one remaining "expensive".
      std::make_tuple(
        "j20", "w1", dynamic(dynamic::object("cheap", 5)),
        ResourceMap{
          {"w1", {{"cheap", 15}, {"expensive", 0}, {"unranked", 0}}},
          {"w2", {{"cheap", 16}, {"expensive", 0}, {"unranked", 1}}},
        }
      ),
      std::make_tuple(
        "j21", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 14}}}}
      ),
      // Eat the last "unranked" from w2
      std::make_tuple(
        "j22", "w2", dynamic(dynamic::object("expensive", 0)),
        ResourceMap{
          {"w1", {{"cheap", 14}, {"expensive", 0}, {"unranked", 0}}},
          {"w2", {{"cheap", 15}, {"expensive", 0}, {"unranked", 0}}},
        }
      ),
      // Now "w1" will have less weight until it completely runs out.
      std::make_tuple(
        "j23", "w1",
        dynamic(dynamic::object("cheap", 12)("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 2}}}}
      ),
      std::make_tuple(
        "j24", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 1}}}}
      ),
      std::make_tuple(
        "j25", "w1", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{{"w1", {{"cheap", 0}}}}
      ),
      // Out of "w1", back to "w2"
      std::make_tuple(
        "j26", "w2", dynamic(dynamic::object("expensive", 0)("unranked", 0)),
        ResourceMap{
          {"w1", {{"cheap", 0}, {"expensive", 0}, {"unranked", 0}}},
          {"w2", {{"cheap", 14}, {"expensive", 0}, {"unranked", 0}}},
        }
      ),
    }
  );
}

TEST_F(TestRemoteRunner, WorkerPhysicalResources) {
  const auto kConfig = std::make_shared<Config>(dynamic::object
    ("enabled", true)
    ("nodes", dynamic::object("levels", dynamic::array()))
    ("resources", dynamic::object
      ("worker", dynamic::object
        // Weight 1 so the "busiest" selector has a sort key.
        // Limit 0 so that no tasks start without physical resources.
        ("ram", dynamic::object("default", 3)("weight", 1)("limit", 0))
        ("cpu", dynamic::object("default", 2)("weight", 1)("limit", 0))
        ("gpu", dynamic::object("default", 1)("weight", 1)("limit", 0))
        // The model-specific GPU resource isn't required by default.
        ("GPU: a", dynamic::object("default", 0)("weight", 0)("limit", 0))
      )
    )
    (kPhysicalResources, dynamic::object
      (kRamMB, dynamic::object
        (kLogicalResource, "ram")
        (kMultiplyLogicalBy, 2)
        // After subtracting reserves, w3 will be too small to run anything.
        (kPhysicalReserveAmount, 1)
      )
      (kCPUCore, dynamic::object(kLogicalResource, "cpu"))
      (kGPUCard, dynamic::object(kLogicalResource, "gpu"))
    )
    // w2 will be smaller, so we will use it first.
    ("remote_worker_selector", "busiest")
  );

  // Like in HandleResources, make the workers before making the runner.
  const auto w1_id = randInstanceID();
  FakeBistroWorkerThread worker1("w1", [&w1_id](cpp2::BistroWorker* w) {
    w->id = w1_id;
    w->usableResources.memoryMB = 10;
    w->usableResources.cpuCores = 2;
    w->usableResources.gpus.emplace_back();
    w->usableResources.gpus.back().name = "a";
  });
  const auto w2_id = randInstanceID();
  FakeBistroWorkerThread worker2("w2", [&w2_id](cpp2::BistroWorker* w) {
    w->id = w2_id;
    w->usableResources.memoryMB = 8;
    w->usableResources.cpuCores = 2;
    w->usableResources.gpus.emplace_back();
    w->usableResources.gpus.back().name = "a";
  });
  const auto w3_id = randInstanceID();
  FakeBistroWorkerThread worker3("w3", [&w3_id](cpp2::BistroWorker* w) {
    w->id = w3_id;
    w->usableResources.memoryMB = 5;  // Minus 1 reserve, over 2 => 2 logical
    w->usableResources.cpuCores = 2;
    w->usableResources.gpus.emplace_back();
    // We have no model-specific resource "GPU: b", so it won't show up.
    w->usableResources.gpus.back().name = "b";
  });

  auto task_statuses =
    std::make_shared<TaskStatuses>(std::make_shared<NoOpTaskStore>());
  RemoteWorkerRunner runner(task_statuses, std::shared_ptr<Monitor>());

  cpp2::WorkerSetID wsid;
  wsid.schedulerID = runner.getSchedulerID();
  wsid = addFakeWorker(&runner, kConfig, worker1, wsid);
  // Fake that w1 knows about w2, for consensusPermitsBecomingHealthy.
  runner.processWorkerHeartbeat(
    worker1.getBistroWorker(), addToWorkerSetID(wsid, worker2),
    RemoteWorkerUpdate()
  );
  wsid = addFakeWorker(&runner, kConfig, worker2, wsid);
  runner.processWorkerHeartbeat(
    worker2.getBistroWorker(), addToWorkerSetID(wsid, worker3),
    RemoteWorkerUpdate()
  );
  wsid = addFakeWorker(&runner, kConfig, worker3, wsid);

  waitToExitInitialWait(runner);
  checkTasksRunOnWorkersLeavingResources(
    kConfig,
    &runner,
    {
      // This fits on either worker, but w2 started slightly smaller.
      std::make_tuple(
        "j1", "w2", dynamic(dynamic::object),
        ResourceMap{  // RAM is 1 lower due to the reserve setting.
          // Less busy than w2.
          {"w1", {{"ram", 4}, {"cpu", 2}, {"gpu", 1}, {"GPU: a", 1}}},
          // Used by the task.
          {"w2", {{"ram", 0}, {"cpu", 0}, {"gpu", 0}, {"GPU: a", 1}}},
          // Too small for task.
          {"w3", {{"ram", 2}, {"cpu", 2}, {"gpu", 1}, {"GPU: a", 0}}},
        }
      ),
    }
  );
}
