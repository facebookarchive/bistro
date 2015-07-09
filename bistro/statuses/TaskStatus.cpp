/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/statuses/TaskStatus.h"

#include <folly/json.h>
#include <folly/Memory.h>
#include <folly/Optional.h>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

TaskStatus TaskStatus::running() {
  return TaskStatus(TaskStatusBits::Running);
}

TaskStatus TaskStatus::running(TaskStatus::DataPtr&& d) {
  return TaskStatus(TaskStatusBits::Running, std::move(d));
}

TaskStatus TaskStatus::done() {
  return TaskStatus(TaskStatusBits::Done);
}

TaskStatus TaskStatus::done(TaskStatus::DataPtr&& d) {
  return TaskStatus(TaskStatusBits::Done, std::move(d));
}

TaskStatus TaskStatus::failed() {
  return TaskStatus(TaskStatusBits::Failed);
}

TaskStatus TaskStatus::failed(TaskStatus::DataPtr&& d) {
  return TaskStatus(TaskStatusBits::Failed, std::move(d));
}

TaskStatus TaskStatus::incomplete(TaskStatus::DataPtr&& d) {
  return TaskStatus(TaskStatusBits::Incomplete, std::move(d));
}

TaskStatus TaskStatus::wasNotRunning() {
  // Does not use or advance backoff!
  return TaskStatus(TaskStatusBits::Error | TaskStatusBits::Overwriteable);
}

TaskStatus TaskStatus::incompleteBackoff(TaskStatus::DataPtr&& d) {
  return TaskStatus(
    // Important: If you remove DoesNotAdvance, you must update update().
    TaskStatusBits::Incomplete | TaskStatusBits::UsesBackoff |
      TaskStatusBits::DoesNotAdvanceBackoff,
    std::move(d)
  );
}

TaskStatus TaskStatus::errorBackoff(TaskStatus::DataPtr&& d) {
  return TaskStatus(
    TaskStatusBits::Error | TaskStatusBits::UsesBackoff,
    std::move(d)
  );
}

TaskStatus TaskStatus::errorBackoff(const std::string& msg) {
  return TaskStatus::errorBackoff(make_unique<dynamic>(
    dynamic::object("exception", msg)
  ));
}

TaskStatus TaskStatus::neverStarted(const std::string& msg) {
  return TaskStatus(
    TaskStatusBits::Error,  // Does not use or advance backoff!
    make_unique<dynamic>(dynamic::object("exception", msg))
  );
}

namespace {

std::unique_ptr<folly::dynamic> makeDataPtr(const folly::dynamic* d) {
  if (d == nullptr || d->empty()) {  // Don't store empties to save RAM
    return nullptr;
  }
  return folly::make_unique<folly::dynamic>(*d);
}

folly::Optional<TaskStatus> fromSimpleLabel(
    const std::string& label,
    const folly::dynamic* d) noexcept {

  if (label == "done") {
    return TaskStatus::done(makeDataPtr(d));
  } else if (label == "incomplete") {
    return TaskStatus::incomplete(makeDataPtr(d));
  } else if (label == "incomplete_backoff") {
    return TaskStatus::incompleteBackoff(makeDataPtr(d));
  } else if (label == "backoff" || label == "error_backoff") {
    return TaskStatus::errorBackoff(makeDataPtr(d));
  } else if (label == "failed") {  // Permanent failure
    return TaskStatus::failed(makeDataPtr(d));
  }
  return folly::none;
}

int64_t bitsToInt(TaskStatusBits b) {
  typedef std::underlying_type<TaskStatusBits>::type EnumType;
  return static_cast<EnumType>(b);
}

TaskStatusBits checkBits(int64_t bits) {
  if (bits & (
    bitsToInt(TaskStatusBits::Unstarted)
      | bitsToInt(TaskStatusBits::Avoided)
      | bitsToInt(TaskStatusBits::Disabled)
  )) {  // These are only used by the Monitor
    throw BistroException("Don't report Unstarted, Avoided, or Disabled bits");
  }

  auto exactly_one = bits & bitsToInt(TaskStatusBits::ExactlyOne);
  if (!exactly_one || (exactly_one & (exactly_one - 1))) {
    throw BistroException("Set exactly one required bit (Running, Done, ...)");
  }

  auto remaining = bits;
  for (auto bit : std::vector<TaskStatusBits>{
    TaskStatusBits::Running,
    TaskStatusBits::Done,
    TaskStatusBits::Incomplete,
    TaskStatusBits::Failed,
    TaskStatusBits::Error,
    TaskStatusBits::UsesBackoff,
    TaskStatusBits::DoesNotAdvanceBackoff,
    TaskStatusBits::Overwriteable,
  }) {
    remaining &= ~bitsToInt(bit);
  }
  if (remaining) {
    throw BistroException("Encountered unknown bit(s): ", remaining);
  }

  return static_cast<TaskStatusBits>(bits);
}

}  // anonymous namespace

TaskStatus TaskStatus::fromString(const std::string& raw_status) noexcept {
  // Check if the string is one of the simple labels
  auto maybe_status = fromSimpleLabel(raw_status, nullptr);
  if (maybe_status.hasValue()) {
    return maybe_status.value();
  }
  // Try to parse it as JSON. Only uses the "result_bits" / "result" and
  // "data" keys -- all others are ignored.  In particular, the "time" key
  // is not used.  We don't want applications to set it.  Recording
  // worker-side completion time would be fine, but due to clock skew, we
  // prefer to use the scheduler timestamp for backoff.
  std::string error;
  try {
    auto d = folly::parseJson(raw_status);  // Throws on bad JSON

    // The "result_bits" protocol can express things like "overwriteable
    // statuses", and is used for worker => scheduler communication.
    if (auto bits_ptr = d.get_ptr("result_bits")) {
      // Throws it "bits" is not an integer, or if they are invalid.
      auto bits = checkBits(bits_ptr->asInt());
      d.erase("result_bits");  // Save RAM on the scheduler
      return TaskStatus(bits, makeDataPtr(d.get_ptr("data")));
    }

    // If "result_bits" are not set, look for a human-readable "result",
    // which is what most tasks will report.
    if (auto result_ptr = d.get_ptr("result")) {
      // Throws if "result" is not a string
      auto result = result_ptr->asString().toStdString();
      d.erase("result");  // Save RAM on the scheduler
      // Is the "result" field a valid label?
      if (auto maybe_status = fromSimpleLabel(result, d.get_ptr("data"))) {
        return maybe_status.value();
      }
    }
    error = "Need a valid \"result\" or \"result_bits\" key";
  } catch (const exception& e) {
    error = e.what();
    // Fall through
  }
  return TaskStatus::errorBackoff(
    folly::to<std::string>("Cannot parse status: ", raw_status, ", ", error)
  );
}

const TaskStatus& TaskStatus::update(
    const cpp2::RunningTask& rt,
    TaskStatus&& new_status) {

  // "DoesNotAdvanceBackoof" should still start with a nonzero backoff, so
  // peek at the next duration if none is set.  For "backoff": ["fail"], it
  // will default to a magic constant set in JobBackoffSettings.cpp.
  auto prev_backoff_duration =
    backoffDuration_ ? backoffDuration_ : rt.nextBackoffDuration.seconds;
  *this = std::move(new_status);
  if (usesBackoff()) {
    if (allSet(bits_, TaskStatusBits::DoesNotAdvanceBackoff)) {
      backoffDuration_ = prev_backoff_duration;
    } else if (rt.nextBackoffDuration.noMoreBackoffs) {
      // We expired our last backoff. Switch to permanently failed.
      if (allSet(bits_, TaskStatusBits::Error)) {
        bits_ =
          replaceBit(bits_, TaskStatusBits::Error, TaskStatusBits::Failed);
      } else {
        // Backoff-to-failure is currently unsupported with other statuses.
        LOG(FATAL) << "Status UsesBackoff, but not DoesNotAdvanceBackoff, "
          << " and lacks Error: " << toJson();
      }
    } else {
      backoffDuration_ = rt.nextBackoffDuration.seconds;
    }
  }
  return *this;
}

dynamic TaskStatus::toDynamicNoTime() const {
  // DO: Consider making this human-readable when a simple label exists?
  // DO: Right now, we log to LogTable::STATUSES with JSON blobs manually
  // constructed to resemble this format.  If this is made human-readable,
  // use toDynamicNoTime() there instead.
  dynamic d = dynamic::object(
    "result_bits",
    static_cast<std::underlying_type<TaskStatusBits>::type>(bits_)
  );
  if (data_) {
    d["data"] = *data_;
  }
  return d;
}

dynamic TaskStatus::toDynamic() const {
  auto d = toDynamicNoTime();
  d["time"] = timestamp_;
  return d;
}

std::string TaskStatus::toJson() const {
  return folly::toJson(toDynamic()).toStdString();
}

}}
