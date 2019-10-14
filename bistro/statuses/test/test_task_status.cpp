/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/experimental/TestUtil.h>
#include <folly/json.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <gtest/gtest.h>

#include "bistro/bistro/statuses/TaskStatus.h"

using namespace facebook::bistro;
using namespace folly;

TEST(TestTaskStatus, Running) {
  auto s = TaskStatus::running();
  EXPECT_TRUE(s.isRunning());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(nullptr, s.dataThreadUnsafe());

  // data on 'running' tasks is only used by TaskStatusObservers.
  auto s2 = TaskStatus::running(std::make_unique<dynamic>("cat"));
  EXPECT_FALSE(s2.isDone());
  EXPECT_TRUE(s2.isRunning());
  EXPECT_EQ(dynamic("cat"), *s2.dataThreadUnsafe());
}

TEST(TestTaskStatus, Done) {
  auto s = TaskStatus::done();
  EXPECT_TRUE(s.isDone());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(nullptr, s.dataThreadUnsafe());

  auto s2 = TaskStatus::done(std::make_unique<dynamic>("cat"));
  EXPECT_TRUE(s2.isDone());
  EXPECT_FALSE(s2.isRunning());
  EXPECT_EQ(dynamic("cat"), *s2.dataThreadUnsafe());
}

TEST(TestTaskStatus, Incomplete) {
  auto s = TaskStatus::incomplete(std::make_unique<dynamic>("cat"));
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(dynamic("cat"), *s.dataThreadUnsafe());
}

TEST(TestTaskStatus, IncompleteBackoff) {
  auto s = TaskStatus::incompleteBackoff(std::make_unique<dynamic>("cat"));
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_TRUE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(dynamic("cat"), *s.dataThreadUnsafe());
}

TEST(TestTaskStatus, WasNotRunning) {
  auto s = TaskStatus::wasNotRunning();
  EXPECT_FALSE(s.isDone());
  // NB: The actual overwriting is only tested in TestTaskStatuses, since
  // that bit is used in TaskStatusSnapshot::updateStatus.
  EXPECT_TRUE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
}

TEST(TestTaskStatus, Error) {
  std::string err = "my error";
  auto s = TaskStatus::errorBackoff(err);
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isFailed());
  EXPECT_TRUE(s.isInBackoff(s.timestamp() - 1));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 1000));
  EXPECT_EQ(err, (*s.dataThreadUnsafe())["exception"].asString());

  auto s2 = TaskStatus::errorBackoff(std::make_unique<dynamic>("cat"));
  EXPECT_FALSE(s2.isRunning());
  EXPECT_FALSE(s2.isDone());
  EXPECT_FALSE(s2.isFailed());
  EXPECT_TRUE(s2.isInBackoff(s.timestamp() - 1));
  EXPECT_FALSE(s2.isInBackoff(s.timestamp() + 1000));
  EXPECT_EQ(dynamic("cat"), *s2.dataThreadUnsafe());
}

TEST(TestTaskStatus, NeverStarted) {
  std::string err = "my error";
  auto s = TaskStatus::neverStarted(err);
  EXPECT_EQ(err, (*s.dataThreadUnsafe())["exception"].asString());
  EXPECT_EQ(TaskStatusBits::Error, s.bits());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isFailed());
  EXPECT_FALSE(s.isInBackoff(s.timestamp() - 1));
}

void updateStatus(TaskStatus* s, TaskStatus&& new_status) {
  JobBackoffSettings jbs(dynamic::array(1, 2, 4, "fail"));
  cpp2::RunningTask rt;
  rt.nextBackoffDuration = jbs.getNext(s->configuredBackoffDuration());
  s->update(rt, std::move(new_status));
}

TEST(TestTaskStatus, Forgiveness) {
  TaskStatus s;
  // Forgive transient backoff
  updateStatus(&s, TaskStatus::errorBackoff("err"));
  EXPECT_TRUE(s.isInBackoff(s.timestamp()));
  s.forgive();
  EXPECT_FALSE(s.isInBackoff(s.timestamp()));
  // Forgive permanent failure
  updateStatus(&s, TaskStatus::failed());
  EXPECT_TRUE(s.isFailed());
  s.forgive();
  EXPECT_FALSE(s.isFailed());
}

TEST(TestTaskStatus, WorkerLostIntoBackoff) {
  const int kEffectiveBackoff = 7;
  const int kConfiguredBackoff = 9;

  // First, try having workerLost() trigger a backoff. Then, run out of
  // backoffs, and have the status become failed.
  for (int backoff_to_fail = 0; backoff_to_fail <= 1; ++backoff_to_fail) {
    auto s = TaskStatus::running();
    cpp2::RunningTask rt;
    rt.nextBackoffDuration.noMoreBackoffs = backoff_to_fail;
    rt.nextBackoffDuration.seconds = kEffectiveBackoff;
    // workerLost() is always used with update()
    s.update(rt, TaskStatus::workerLost("worker1", kConfiguredBackoff));

    // First, check the original status, then forgive() and check again.
    for (int forgive = 0; forgive <= 1; ++forgive) {
      if (forgive) {
        s.forgive();
      }
      auto kSavedBackoff = forgive ? 0 : kConfiguredBackoff;

      EXPECT_FALSE(s.isEmpty());
      EXPECT_FALSE(s.isRunning());
      EXPECT_EQ(backoff_to_fail && !forgive, s.isFailed());
      EXPECT_TRUE(s.isOverwriteable());

      // Check that we stored the configured backoff correctly.
      auto bd = s.configuredBackoffDuration();
      EXPECT_EQ(backoff_to_fail && !forgive, bd.noMoreBackoffs);
      EXPECT_EQ(backoff_to_fail ? 0 : kSavedBackoff, bd.seconds);

      // The effective backoff differs from the configured one, and
      // is NOT forgiven.
      EXPECT_TRUE(s.isInBackoff(s.timestamp() + kEffectiveBackoff - 1));
      EXPECT_FALSE(s.isInBackoff(s.timestamp() + kEffectiveBackoff));

      dynamic expected_data = dynamic::object
        ("worker_shard", "worker1")
        ("exception", "Remote worker lost (crashed? network down?)")
        ("__bistro_saved_backoff", kSavedBackoff);
      EXPECT_EQ(expected_data, *s.dataThreadUnsafe());

      auto d = s.toDynamic();
      EXPECT_GT(d.at("time").getInt(), 0);
      d.erase("time");
      EXPECT_EQ(dynamic(
        dynamic::object
          ("backoff_duration", kEffectiveBackoff)
          ("data", expected_data)
          // Use Thrift defs to check there are no typos in the mapping :-P
          ("result_bits",
            (int)cpp2::BistroTaskStatusBits::HAS_SAVED_BACKOFF
              | (int)cpp2::BistroTaskStatusBits::OVERWRITEABLE
              | (int)cpp2::BistroTaskStatusBits::USES_BACKOFF
              | (
                (backoff_to_fail && !forgive)
                  ? (int)cpp2::BistroTaskStatusBits::FAILED
                  : (int)cpp2::BistroTaskStatusBits::ERROR
              ))
      ), d);
    }
  }
}

TEST(TestTaskStatus, UpdateSimple) {
  auto s = TaskStatus::running();
  EXPECT_TRUE(s.isRunning());
  EXPECT_FALSE(s.isDone());

  updateStatus(&s, TaskStatus::done());
  EXPECT_TRUE(s.isDone());
  EXPECT_FALSE(s.isRunning());
}

TEST(TestTaskStatus, UpdateData) {
  auto s = TaskStatus::running(std::make_unique<dynamic>("foo"));
  EXPECT_EQ(dynamic("foo"), *s.dataThreadUnsafe());

  updateStatus(&s, TaskStatus::errorBackoff(std::make_unique<dynamic>("bar")));
  EXPECT_EQ(dynamic("bar"), *s.dataThreadUnsafe());
}

TEST(TestTaskStatus, BackoffUpdates) {
  auto s = TaskStatus::running();
  EXPECT_FALSE(s.isInBackoff(s.timestamp() - 1));

  // Initial backoff should be 1 second
  updateStatus(&s, TaskStatus::errorBackoff("err"));
  EXPECT_TRUE(s.isInBackoff(s.timestamp() - 1));
  EXPECT_TRUE(s.isInBackoff(s.timestamp()));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 1));

  // Backoff should increase to 2 seconds
  updateStatus(&s, TaskStatus::errorBackoff("err"));
  EXPECT_TRUE(s.isInBackoff(s.timestamp() + 1));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 2));

  // We should reset the backoff on an "incomplete"
  updateStatus(&s, TaskStatus::incomplete(std::make_unique<dynamic>("cat")));
  updateStatus(&s, TaskStatus::errorBackoff("err"));

  // Backoff is 1 second again
  EXPECT_TRUE(s.isInBackoff(s.timestamp() - 1));
  EXPECT_TRUE(s.isInBackoff(s.timestamp()));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 1));

  // Backoff should increase to 2 seconds
  updateStatus(&s, TaskStatus::errorBackoff("err"));
  EXPECT_TRUE(s.isInBackoff(s.timestamp() + 1));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 2));

  // Backoff should increase to 4 seconds
  updateStatus(&s, TaskStatus::errorBackoff("err"));
  EXPECT_TRUE(s.isInBackoff(s.timestamp() + 3));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 4));

  updateStatus(&s, TaskStatus::errorBackoff("err"));

  // "fail" follows 4, so the status should now be permanently failed.
  EXPECT_TRUE(s.isFailed());
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 1000000));
}

TEST(TestTaskStatus, DoesNotAdvanceBackoff) {
  auto s = TaskStatus::running();
  EXPECT_FALSE(s.isInBackoff(s.timestamp() - 1));

  // A DoesNotAdvanceBackoff status in a "no backoff" state defaults to the
  // lowest available backoff, 1 sec in this test.
  updateStatus(&s, TaskStatus::incompleteBackoff(nullptr));
  EXPECT_TRUE(s.isInBackoff(s.timestamp()));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 1));

  // But receiving another DoesNotAdvanceBackoff leaves the duration alone.
  updateStatus(&s, TaskStatus::incompleteBackoff(nullptr));
  EXPECT_TRUE(s.isInBackoff(s.timestamp()));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 1));

  // In contrast, error advances it to 2 seconds.
  updateStatus(&s, TaskStatus::errorBackoff(""));
  EXPECT_TRUE(s.isInBackoff(s.timestamp() + 1));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 2));

  // And yet another DoesNotAdvanceBackoff again leaves the duration alone.
  updateStatus(&s, TaskStatus::incompleteBackoff(nullptr));
  EXPECT_TRUE(s.isInBackoff(s.timestamp() + 1));
  EXPECT_FALSE(s.isInBackoff(s.timestamp() + 2));
}

TaskStatus expectStatusFromString(const TaskStatus& expected, const char* s) {
  auto actual = TaskStatus::fromString(s);
  EXPECT_NEAR(expected.timestamp(), actual.timestamp(), 2);
  EXPECT_EQ(expected.toDynamicNoTime(), actual.toDynamicNoTime());
  return actual;
}

TaskStatus expectStatusFromString(
    const TaskStatus& expected,
    const folly::dynamic& d) {

  auto s = folly::toJson(d);
  return expectStatusFromString(expected, s.c_str());
}

void expectErrorFromString(const std::string& error_pcre, const char* s) {
  auto expected = TaskStatus::errorBackoff("");
  auto actual = TaskStatus::fromString(s);
  EXPECT_NEAR(expected.timestamp(), actual.timestamp(), 2);

  auto expected_d = expected.toDynamicNoTime();
  expected_d["data"].erase("exception");

  auto actual_d = actual.toDynamicNoTime();
  folly::Optional<folly::fbstring> maybe_exception;
  try {
    maybe_exception = actual_d["data"]["exception"].asString();
  } catch (const std::exception& e) {
    ADD_FAILURE() << "No 'data' with 'exception' in "
      << folly::toJson(actual_d);
  }
  if (maybe_exception) {
    EXPECT_PCRE_MATCH(error_pcre, maybe_exception.value());
    actual_d["data"].erase("exception");
  }

  EXPECT_EQ(expected_d, actual_d);
}

void expectErrorFromString(const std::string& pcre, const folly::dynamic& d) {
  auto s = folly::toJson(d);
  expectErrorFromString(pcre, s.c_str());
}

std::unique_ptr<folly::dynamic> makeDataPtr(const folly::dynamic& d) {
  return std::make_unique<folly::dynamic>(d);
}

// This HasSavedBackoff bit, and how it uses "data" is a private
// implementation detail, so don't expose it via `fromString`.
TEST(TestTaskStatus, NoWorkerLostFromString) {
  expectErrorFromString(
    ".* Don't report Unstarted, Avoided, Disabled, or HasSavedBackoff bits",
    dynamic::object(
      "result_bits",
      static_cast<int>(cpp2::BistroTaskStatusBits::HAS_SAVED_BACKOFF)
    )
  );
  // "__bistro_saved_backoff" is silently removed from "data"
  expectStatusFromString(
    TaskStatus::done(),
    dynamic::object
      ("data", dynamic::object("__bistro_saved_backoff", 123))
      ("result_bits", static_cast<int>(TaskStatusBits::Done))
  );
}

TEST(TestTaskStatus, ToAndFromString) {
  auto null_data = []() { return std::shared_ptr<folly::dynamic>(); };

  // Simple string statuses
  expectStatusFromString(TaskStatus::done(), "done");
  expectStatusFromString(TaskStatus::incomplete(null_data()), "incomplete");
  expectStatusFromString(
    TaskStatus::incompleteBackoff(null_data()), "incomplete_backoff"
  );
  expectStatusFromString(TaskStatus::errorBackoff(null_data()), "backoff");
  expectStatusFromString(TaskStatus::errorBackoff(null_data()), "error_backoff");
  expectStatusFromString(TaskStatus::failed(null_data()), "failed");

  // Unknown non-JSON strings
  expectErrorFromString("Cannot parse status:.* expected json value", "");
  expectErrorFromString("Cannot parse status:.* expected json value", "foo");

  // Invalid "result_bits"
  expectErrorFromString(
    ".* Invalid leading character.*: .bad.",
    dynamic::object("result_bits", "bad")
  );
  expectErrorFromString(
    ".* TypeError: .*, but had type `array'",
    dynamic::object("result_bits", dynamic::array())
  );
  expectErrorFromString(
    ".* Set exactly one required bit .*", dynamic::object("result_bits", 0)
  );
  for (auto bit : std::vector<TaskStatusBits>{
    TaskStatusBits::Unstarted,
    TaskStatusBits::Avoided,
    TaskStatusBits::Disabled,
  }) {
    expectErrorFromString(
      ".* Don't report Unstarted, Avoided, Disabled, or HasSavedBackoff bits",
      dynamic::object("result_bits", static_cast<int>(bit))
    );
  }
  std::vector<TaskStatusBits> exactly_one = {
    TaskStatusBits::Running,
    TaskStatusBits::Done,
    TaskStatusBits::Incomplete,
    TaskStatusBits::Failed,
    TaskStatusBits::Error,
  };
  for (auto bit1 : exactly_one) {
    for (auto bit2 : exactly_one) {
     if (bit1 != bit2) {
        expectErrorFromString(
          ".* Set exactly one required bit .*",
          dynamic::object("result_bits", static_cast<int>(bit1 | bit2))
        );
      }
    }
  }
  expectErrorFromString(
    ".* Encountered unknown bit\\(s\\): 4096",
    dynamic::object("result_bits", 4096 | 0x2)
  );

  // Valid "result_bits" with no data -- cover all no-data constructors
  for (const auto& p : std::vector<std::pair<TaskStatus, TaskStatusBits>>{
    {TaskStatus::done(), TaskStatusBits::Done},
    {TaskStatus::incomplete(null_data()), TaskStatusBits::Incomplete},
    {
      TaskStatus::incompleteBackoff(null_data()),
      TaskStatusBits::Incomplete | TaskStatusBits::UsesBackoff |
        TaskStatusBits::DoesNotAdvanceBackoff
    },
    {
      TaskStatus::errorBackoff(null_data()),
      TaskStatusBits::Error | TaskStatusBits::UsesBackoff
    },
    {TaskStatus::failed(), TaskStatusBits::Failed},
    {
      TaskStatus::wasNotRunning(),
      TaskStatusBits::Error | TaskStatusBits::Overwriteable
    },
  }) {
    auto status = expectStatusFromString(
      p.first, dynamic::object("result_bits", static_cast<int>(p.second))
    );
    EXPECT_EQ(allSet(p.second, TaskStatusBits::Running), status.isRunning());
    EXPECT_EQ(allSet(p.second, TaskStatusBits::Done), status.isDone());
    EXPECT_EQ(allSet(p.second, TaskStatusBits::Failed), status.isFailed());
    EXPECT_EQ(
      allSet(p.second, TaskStatusBits::Overwriteable),
      status.isOverwriteable()
    );
  }

  // "data" must be an object.
  expectErrorFromString(
    ".* TypeError: expected dynamic type `object', but had type `array'.*",
    dynamic::object
      ("data", dynamic::array(5, "6"))
      ("result_bits", static_cast<int>(TaskStatusBits::Failed))
  );

  // Try some valid "result_bits" with non-empty data
  expectStatusFromString(
    TaskStatus::incomplete(makeDataPtr(
      dynamic::object("boof", dynamic::array())
    )),
    dynamic::object
      ("data", dynamic::object("boof", dynamic::array()))
      ("result_bits", static_cast<int>(TaskStatusBits::Incomplete))
  );
  expectStatusFromString(
    TaskStatus::failed(makeDataPtr(dynamic::object("k", "v"))),
    dynamic::object
      ("data", dynamic::object("k", "v"))
      ("result_bits", static_cast<int>(TaskStatusBits::Failed))
  );

  // We don't store empty data values to save RAM
  expectStatusFromString(
    TaskStatus::failed(),
    dynamic::object
      ("data", dynamic::object())
      ("result_bits", static_cast<int>(TaskStatusBits::Failed))
  );

  // Invalid "result" field
  expectErrorFromString(
    ".* TypeError: .*, but had type `array'",
    dynamic::object("result", dynamic::array())
  );
  expectErrorFromString(
    ".* Need a valid \"result\" .*",
    dynamic::object("result", "potato")
  );

  // JSON has neither a "result_bits" nor a "result" field
  expectErrorFromString(
    ".* Need a valid \"result\" or \"result_bits\" .*",
    dynamic::object()
  );

  // Valid "result" fields, with and without data
  expectStatusFromString(
    TaskStatus::failed(makeDataPtr(dynamic::object("k", "v"))),
    dynamic::object("data", dynamic::object("k", "v"))("result", "failed")
  );
  expectStatusFromString(
    TaskStatus::done(),
    dynamic::object("result", "done")
  );
}
