/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
  EXPECT_EQ(nullptr, s.data());

  // data on 'running' tasks is only used by TaskStatusObservers.
  auto s2 = TaskStatus::running(make_unique<dynamic>("cat"));
  EXPECT_FALSE(s2.isDone());
  EXPECT_TRUE(s2.isRunning());
  EXPECT_EQ(dynamic("cat"), *s2.data());
}

TEST(TestTaskStatus, Done) {
  auto s = TaskStatus::done();
  EXPECT_TRUE(s.isDone());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(nullptr, s.data());

  auto s2 = TaskStatus::done(make_unique<dynamic>("cat"));
  EXPECT_TRUE(s2.isDone());
  EXPECT_FALSE(s2.isRunning());
  EXPECT_EQ(dynamic("cat"), *s2.data());
}

TEST(TestTaskStatus, Incomplete) {
  auto s = TaskStatus::incomplete(make_unique<dynamic>("cat"));
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(dynamic("cat"), *s.data());
}

TEST(TestTaskStatus, IncompleteBackoff) {
  auto s = TaskStatus::incompleteBackoff(make_unique<dynamic>("cat"));
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_TRUE(s.isInBackoff(0));
  EXPECT_FALSE(s.isFailed());
  EXPECT_EQ(dynamic("cat"), *s.data());
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
  EXPECT_EQ(err, (*s.data())["exception"].asString().toStdString());

  auto s2 = TaskStatus::errorBackoff(make_unique<dynamic>("cat"));
  EXPECT_FALSE(s2.isRunning());
  EXPECT_FALSE(s2.isDone());
  EXPECT_FALSE(s2.isFailed());
  EXPECT_TRUE(s2.isInBackoff(s.timestamp() - 1));
  EXPECT_FALSE(s2.isInBackoff(s.timestamp() + 1000));
  EXPECT_EQ(dynamic("cat"), *s2.data());
}

TEST(TestTaskStatus, NeverStarted) {
  std::string err = "my error";
  auto s = TaskStatus::neverStarted(err);
  EXPECT_EQ(err, (*s.data())["exception"].asString().toStdString());
  EXPECT_EQ(TaskStatusBits::Error, s.bits());
  EXPECT_FALSE(s.isOverwriteable());
  EXPECT_FALSE(s.isRunning());
  EXPECT_FALSE(s.isDone());
  EXPECT_FALSE(s.isFailed());
  EXPECT_FALSE(s.isInBackoff(s.timestamp() - 1));
}

void updateStatus(TaskStatus* s, TaskStatus&& new_status) {
  JobBackoffSettings jbs(dynamic{ 1, 2, 4, "fail" });
  cpp2::RunningTask rt;
  rt.nextBackoffDuration = jbs.getNext(s->backoffDuration());
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

TEST(TestTaskStatus, UpdateSimple) {
  auto s = TaskStatus::running();
  EXPECT_TRUE(s.isRunning());
  EXPECT_FALSE(s.isDone());

  updateStatus(&s, TaskStatus::done());
  EXPECT_TRUE(s.isDone());
  EXPECT_FALSE(s.isRunning());
}

TEST(TestTaskStatus, UpdateData) {
  auto s = TaskStatus::running(make_unique<dynamic>("foo"));
  EXPECT_EQ(dynamic("foo"), *s.data());

  updateStatus(&s, TaskStatus::errorBackoff(make_unique<dynamic>("bar")));
  EXPECT_EQ(dynamic("bar"), *s.data());
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
  updateStatus(&s, TaskStatus::incomplete(make_unique<dynamic>("cat")));
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
    EXPECT_TRUE(false) << "No 'data' with 'exception' in "
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
  return folly::make_unique<folly::dynamic>(d);
}

TEST(TestTaskStatus, ToAndFromString) {
  std::shared_ptr<folly::dynamic> null_data;

  // Simple string statuses
  expectStatusFromString(TaskStatus::done(), "done");
  expectStatusFromString(TaskStatus::incomplete(null_data), "incomplete");
  expectStatusFromString(
    TaskStatus::incompleteBackoff(null_data), "incomplete_backoff"
  );
  expectStatusFromString(TaskStatus::errorBackoff(null_data), "backoff");
  expectStatusFromString(TaskStatus::errorBackoff(null_data), "error_backoff");
  expectStatusFromString(TaskStatus::failed(null_data), "failed");

  // Unknown non-JSON strings
  expectErrorFromString("Cannot parse status:.* expected json value", "");
  expectErrorFromString("Cannot parse status:.* expected json value", "foo");

  // Invalid "result_bits"
  expectErrorFromString(
    ".* Invalid leading character in conversion to integral: 'bad'",
    dynamic::object("result_bits", "bad")
  );
  expectErrorFromString(
    ".* TypeError: .*, but had type `array'",
    dynamic::object("result_bits", {})
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
      ".* Don't report Unstarted, Avoided, or Disabled bits",
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
    ".* Encountered unknown bit\\(s\\): 2048",
    dynamic::object("result_bits", 0x800 | 0x2)
  );

  // Valid "result_bits" with no data -- cover all no-data constructors
  for (const auto& p : std::vector<std::pair<TaskStatus, TaskStatusBits>>{
    {TaskStatus::done(), TaskStatusBits::Done},
    {TaskStatus::incomplete(null_data), TaskStatusBits::Incomplete},
    {
      TaskStatus::incompleteBackoff(null_data),
      TaskStatusBits::Incomplete | TaskStatusBits::UsesBackoff |
        TaskStatusBits::DoesNotAdvanceBackoff
    },
    {
      TaskStatus::errorBackoff(null_data),
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

  // Try some valid "result_bits" with non-empty data
  expectStatusFromString(
    TaskStatus::incomplete(makeDataPtr(dynamic::object("boof", {}))),
    dynamic::object
      ("data", dynamic::object("boof", {}))
      ("result_bits", static_cast<int>(TaskStatusBits::Incomplete))
  );
  expectStatusFromString(
    TaskStatus::failed(makeDataPtr(dynamic{5, "6"})),
    dynamic::object
      ("data", {5, "6"})
      ("result_bits", static_cast<int>(TaskStatusBits::Failed))
  );

  // We don't store empty data values to save RAM
  expectStatusFromString(
    TaskStatus::failed(),
    dynamic::object
      ("data", {})
      ("result_bits", static_cast<int>(TaskStatusBits::Failed))
  );

  // Invalid "result" field
  expectErrorFromString(
    ".* TypeError: .*, but had type `array'",
    dynamic::object("result", {})
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
    TaskStatus::failed(makeDataPtr(dynamic{5, "6"})),
    dynamic::object("data", {5, "6"})("result", "failed")
  );
  expectStatusFromString(
    TaskStatus::done(),
    dynamic::object("result", "done")
  );
}
