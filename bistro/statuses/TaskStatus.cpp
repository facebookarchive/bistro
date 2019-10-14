/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/statuses/TaskStatus.h"

#include <folly/json.h>
#include <folly/Memory.h>
#include <folly/Optional.h>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {
constexpr folly::StringPiece kBistroSavedBackoff = "__bistro_saved_backoff";
constexpr folly::StringPiece kException = "exception";
constexpr folly::StringPiece kData = "data";
constexpr folly::StringPiece kResult = "result";
constexpr folly::StringPiece kResultBits = "result_bits";
}  // anonymous namespace

TaskStatus TaskStatus::running() {
  return TaskStatus(TaskStatusBits::Running);
}

TaskStatus TaskStatus::running(TaskStatus::DataPtr&& d) {
  return TaskStatus(TaskStatusBits::Running, std::move(d));
}

TaskStatus TaskStatus::done() {
  return TaskStatus(TaskStatusBits::Done);
}

TaskStatus TaskStatus::done(time_t timestamp) {
  return TaskStatus(TaskStatusBits::Done, timestamp);
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

TaskStatus TaskStatus::workerLost(std::string shard, uint32_t saved_backoff) {
  // This **will** decrease the retry count, which makes sense since the
  // worker might have crashed **because** of this task (unlike
  // neverStarted()).
  return TaskStatus(
    TaskStatusBits::Error
      | TaskStatusBits::UsesBackoff
      // IMPORTANT: If you want to use this in any other status, be aware
      // that TaskStatus::update() **only** saves the backoff duration from
      // the RunningTask if the new status `usesBackoff()`, so you will need
      // that bit too. ::forgive() also relies on this.
      | TaskStatusBits::HasSavedBackoff  // docs in the .h file
      // Ensure that application-specific statuses (if they arrive while
      // the worker is MUST_DIE but still associated) can overwrite the
      // 'lost' status, but the 'lost' status cannot overwrite a real
      // status that arrived between loseRunningTasks and this call.
      | TaskStatusBits::Overwriteable,
    std::make_unique<dynamic>(dynamic::object
      ("worker_shard", shard)
      (kException, "Remote worker lost (crashed? network down?)")
      (kBistroSavedBackoff, saved_backoff)
    )
  );
}

TaskStatus TaskStatus::errorBackoff(const std::string& msg) {
  return TaskStatus::errorBackoff(make_unique<dynamic>(
    dynamic::object(kException, msg)
  ));
}

TaskStatus TaskStatus::neverStarted(const std::string& msg) {
  return TaskStatus(
    TaskStatusBits::Error,  // Does not use or advance backoff!
    make_unique<dynamic>(dynamic::object(kException, msg))
  );
}

namespace {

std::unique_ptr<folly::dynamic> makeDataPtr(const folly::dynamic* d) {
  if (d == nullptr || d->empty()) {  // Don't store empties to save RAM
    return nullptr;
  }
  return std::make_unique<folly::dynamic>(*d);
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
    // These are only used by the Monitor
    bitsToInt(TaskStatusBits::Unstarted)
      | bitsToInt(TaskStatusBits::Avoided)
      | bitsToInt(TaskStatusBits::Disabled)
      // Private to the TaskStatus implementation
      | bitsToInt(TaskStatusBits::HasSavedBackoff)
  )) {
    throw BistroException(
      "Don't report Unstarted, Avoided, Disabled, or HasSavedBackoff bits"
    );
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
    // Disallowed above, since it's private: TaskStatusBits::HasSavedBackoff
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
  if (auto maybe_status = fromSimpleLabel(raw_status, nullptr)) {
    return maybe_status.value();
  }
  // Try to parse it as JSON. Only uses the kResultBits / kResult and
  // kData keys -- all others are ignored.  In particular, the "time" key
  // is not used.  We don't want applications to set it.  Recording
  // worker-side completion time would be fine, but due to clock skew, we
  // prefer to use the scheduler timestamp for backoff.
  //
  // "backoff_duration" is also ignored, because at the moment, it is not
  // supported for tasks to tinker with their own backoffs.
  std::string error;
  try {
    auto d = folly::parseJson(raw_status);  // Throws on bad JSON
    if (auto data = d.get_ptr(kData)) {
      // Don't allow the task to tinker with saved backoffs, since this is a
      // private implementation detail (and checkBits forbids the bit).
      data->erase(kBistroSavedBackoff);
    }
    // The kResultBits protocol can express things like "overwriteable
    // statuses", and is used for worker => scheduler communication.
    if (auto bits_ptr = d.get_ptr(kResultBits)) {
      // Throws it "bits" is not an integer, or if they are invalid.
      auto bits = checkBits(bits_ptr->asInt());
      d.erase(kResultBits);  // Save RAM on the scheduler
      return TaskStatus(bits, makeDataPtr(d.get_ptr(kData)));
    }

    // If kResultBits are not set, look for a human-readable kResult,
    // which is what most tasks will report.
    if (auto result_ptr = d.get_ptr(kResult)) {
      // Throws if kResult's value is not a string
      auto result = result_ptr->asString();
      d.erase(kResult);  // Save RAM on the scheduler
      // Is the kResult field a valid label?
      if (auto maybe_status = fromSimpleLabel(result, d.get_ptr(kData))) {
        return maybe_status.value();
      }
    }
    error = folly::to<std::string>(
      "Need a valid \"", kResult, "\" or \"", kResultBits, "\" key"
    );
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

  // Updating with a "DoesNotAdvanceBackoff" status should use the saved
  // backoff value, rather than the extended one required for lost workers,
  // because that's the job's authentic configuration.
  auto prev_backoff_duration = hasSavedBackoff()
    ? data_->at(kBistroSavedBackoff).getInt()
    // "DoesNotAdvanceBackoff" needs to start with a nonzero backoff, so
    // peek at the next duration if none is set.  For "backoff": ["fail"],
    // it will default to a magic constant set in JobBackoffSettings.cpp.
    : (backoffDuration_ ? backoffDuration_ : rt.nextBackoffDuration.seconds);

  *this = std::move(new_status);
  if (usesBackoff()) {
    if (allSet(bits_, TaskStatusBits::DoesNotAdvanceBackoff)) {
      backoffDuration_ = prev_backoff_duration;
    } else {
      // Store the specified duration regardless of whether the task is
      // about to become "failed", since this protects us from starting a
      // second task too soon in this event sequence:
      //   (1) a task fails because a worker is lost,
      //   (2) the task is forgiven, promoting it to Error, which would
      //       let it run immediately
      // See forgive() and the lostRunningTasks() handler for the details.
      backoffDuration_ = rt.nextBackoffDuration.seconds;
      if (rt.nextBackoffDuration.noMoreBackoffs) {
        // We expired our last backoff. Switch to permanently failed.
        if (allSet(bits_, TaskStatusBits::Error)) {
          bits_ =
            replaceBit(bits_, TaskStatusBits::Error, TaskStatusBits::Failed);
        } else {
          // Backoff-to-failure is currently unsupported with other statuses.
          LOG(FATAL) << "Status UsesBackoff, but not DoesNotAdvanceBackoff, "
            << " and lacks Error: " << toJson();
        }
      }
    }
  }
  return *this;
}

void TaskStatus::forgive() {
  if (isFailed()) {
    bits_ = replaceBit(bits_, TaskStatusBits::Failed, TaskStatusBits::Error);
  }
  if (usesBackoff()) {
    // Don't allow truncating workerLost() backoffs, since these exist to
    // protect the scheduler from starting a second task while the lost one is
    // still terminating -- see comments in the lostRunningTasks() handler.
    if (hasSavedBackoff()) {
      if (data_) {
        data_->at(kBistroSavedBackoff) = 0;
      }
    } else {
      backoffDuration_ = 0;
    }
  }
}

cpp2::BackoffDuration TaskStatus::configuredBackoffDuration() const {
  cpp2::BackoffDuration bd;
  if (isFailed()) {
    bd.noMoreBackoffs = true;
    // It's probably OK not to restore any saved backoff here.
    return bd;
  }
  bd.noMoreBackoffs = false;
  bd.seconds = hasSavedBackoff()
    // backoffDuration_ may not have the job-specified value, but instead a
    // larger one mandated by the lostRunningTasks() handler.
    ? data_->at(kBistroSavedBackoff).getInt()
    : backoffDuration_;
  return bd;
}

dynamic TaskStatus::toDynamicNoTime() const {
  // DO: Consider making this human-readable when a simple label exists?
  dynamic d = dynamic::object(
    kResultBits,
    static_cast<std::underlying_type<TaskStatusBits>::type>(bits_)
  );
  if (data_) {
    d[kData] = *data_;
  }
  return d;
}

dynamic TaskStatus::toDynamic() const {
  auto d = toDynamicNoTime();
  d["time"] = timestamp_;
  // If there is a saved backoff, it will get exported with kData, while the
  // bit will be in kResultBits.
  if (usesBackoff() && backoffDuration_ != 0) {
    d["backoff_duration"] = backoffDuration_;
  }
  return d;
}

std::string TaskStatus::toJson() const {
  return folly::toJson(toDynamic());
}

}}
