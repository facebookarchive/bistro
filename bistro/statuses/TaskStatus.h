/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/dynamic.h>
#include "bistro/bistro/config/JobBackoffSettings.h"
#include "bistro/bistro/if/gen-cpp2/bits_types.h"

namespace facebook { namespace bistro {

/**
 * Bits that represent the status of a task. A task can have a combination of
 * these. A type-safe version of cpp2::BistroTaskStatusBits.
 *
 * IMPORTANT: The "data" field must be a folly::dynamic::object.
 * Future: enforce this nicely in the constructors.
 */
enum class TaskStatusBits : unsigned short {
  Empty = 0x0,

  /**
   * These bits represent the task's current status.
   */

  // Used **only** by the monitor to report that Bistro has not yet tried to
  // run the task (exclusive with other bits).  The rest of Bistro simply
  // leaves all bits empty.
  Unstarted =
    static_cast<unsigned short>(cpp2::BistroTaskStatusBits::UNSTARTED),

  // The task is currently running
  Running = static_cast<unsigned short>(cpp2::BistroTaskStatusBits::RUNNING),
  Done = static_cast<unsigned short>(
    cpp2::BistroTaskStatusBits::DONE), // The task has completed
  // We ran the task and it returned incomplete -- no error, can run again
  Incomplete =
    static_cast<unsigned short>(cpp2::BistroTaskStatusBits::INCOMPLETE),
  // This task has failed permanently (we won't run it again)
  Failed = static_cast<unsigned short>(cpp2::BistroTaskStatusBits::FAILED),
  // The task ran, and exited with an error, but may run again
  Error = static_cast<unsigned short>(cpp2::BistroTaskStatusBits::ERROR),

  // Exactly one of the above status bits must be set.
  ExactlyOne = Unstarted | Running | Done | Incomplete | Failed | Error,

  // The task waits for a period of time before trying again.  Unless
  // DoesNotAdvanceBackoff is also set, advances the task to the next
  // backoff duration (or to permanent failure once backoffs run out).
  // CAUTION: Currently, update() assumes that UsesBackoff can only occur
  // without DoesNotAdvanceBackoff if Error is set.
  //
  // The monitor uses this bit to report tasks that are in backoff now.
  UsesBackoff =
    static_cast<unsigned short>(cpp2::BistroTaskStatusBits::USES_BACKOFF),
  // When combined with UsesBackoff, does not advance the index into the
  // backoff array, but simply reuses the current one.  It is used:
  //  - For the "incomplete_backoff" status, so it never becomes "failed".
  //  - When the scheduler or worker, for whatever reason, do not actually
  //    execute the task that was slated to run.
  //  - In responses to notifyIfTasksNotRunning, together with Overwriteable.
  DoesNotAdvanceBackoff =static_cast<unsigned short>(
    cpp2::BistroTaskStatusBits::DOES_NOT_ADVANCE_BACKOFF),

  // Indicates that this status should be overwritten by the next "not
  // running" status to arrive for the same task.  This is used with remote
  // workers.  A task may becomes "not running" due to a synthetic status
  // that is *not* the real application-generated status, but that may race
  // the real status update.  Setting this flag on the synthetic status
  // ensures that TaskStatuses stores the true status instead, regardless of
  // their order of arrival.  The use cases are:
  //  - After a runTask partial failure, the scheduler will query the
  //    worker to see if the task is actually running. The synthetic "not
  //    running" needs to be marked in case a real updateStatus arrives.
  //  - When a worker is lost, its lost tasks *may* concurrently get real
  //    status updates from the worker, so the synthetic "task lost"
  //    status must be marked.
  Overwriteable =
    static_cast<unsigned short>(cpp2::BistroTaskStatusBits::OVERWRITEABLE),

  // This is a private status bit, which is used to deal with the fact that
  // tasks, which failed because their worker got lost, need to store two
  // distinct backoff durations -- the job-configured backoff value is needed
  // so that JobBackoffSettings::getNext can work, while the effective
  // backoff value may need to be longer to prevent the scheduler from
  // accidentally starting a second task while the first one is still being
  // killed by the worker.
  //
  // So why add this bit? It tells TaskStatus internals that the configured
  // backoff may differ from the effective `backoffDuration_`, meaning that
  // we have to access the "__bistro_saved_backoff" field in `data` to find
  // the configured backoff duration.  The alternative to this scheme would
  // be to store both the configured and the effective backoff duration on
  // every TaskStatus, which would significantly increase Bistro's memory
  // footprint.  Since the number of lost-worker tasks should be small, this
  // is a good trade of CPU to gain memory.
  HasSavedBackoff =
    static_cast<unsigned short>(cpp2::BistroTaskStatusBits::HAS_SAVED_BACKOFF),

  /**
   * These bits are used **only** by the monitor to represent permanent
   * facts about the task and whether it can run or not.  They are known
   * before we've ever run a task.
   */

  // The job doesn't want to run on the node
  Avoided = static_cast<unsigned short>(cpp2::BistroTaskStatusBits::AVOIDED),
  Disabled =static_cast<unsigned short>(
    cpp2::BistroTaskStatusBits::DISABLED),  // The node is disabled
};

/**
 * Operator to combine two bits in a typesafe manner.
 */
constexpr TaskStatusBits operator|(TaskStatusBits a, TaskStatusBits b) {
  typedef std::underlying_type<TaskStatusBits>::type EnumType;
  return static_cast<TaskStatusBits>(
    static_cast<EnumType>(a) | static_cast<EnumType>(b)
  );
}

/**
 * Operator to invert TaskStatusBits in a typesafe manner.
 */
constexpr TaskStatusBits operator~(TaskStatusBits a) {
  typedef std::underlying_type<TaskStatusBits>::type EnumType;
  return static_cast<TaskStatusBits>(~static_cast<EnumType>(a));
}

/**
 * Operator to check for the presence of a set bit.
 */
constexpr TaskStatusBits operator&(TaskStatusBits a, TaskStatusBits b) {
  typedef std::underlying_type<TaskStatusBits>::type EnumType;
  return static_cast<TaskStatusBits>(
    static_cast<EnumType>(a) & static_cast<EnumType>(b)
  );
}

constexpr bool allSet(TaskStatusBits where, TaskStatusBits which_bits) {
  return (where & which_bits) == which_bits;
}

inline TaskStatusBits replaceBit(
    TaskStatusBits where,
    TaskStatusBits what,
    TaskStatusBits with) {

  typedef std::underlying_type<TaskStatusBits>::type EnumType;
  return static_cast<TaskStatusBits>(
    static_cast<EnumType>(with)
      | (static_cast<EnumType>(where) & ~static_cast<EnumType>(what))
  );
}

/**
 * Represents a task status. We store a lot of these in memory so it's important
 * to keep its size as low as possible.
 */
class TaskStatus {

  // This is not a pointer to a const because `forgive` mutates some
  // implementation detail of the `data`.  However, the user-supplied data
  // is logically const.  There would be no thread-safety benefits to making
  // this const, since calling a mutator update() and forgive() concurrently
  // with any read access the TaskStatus subject to races anyway.  Just copy
  // the TaskStatus under a lock if you need safety.
  //
  // This wastes 8 bytes per TaskStatus, but makes this class trivially
  // copiable.  If you want to go on a memory usage crusade, change this to
  // a unique_ptr and make a copy constructor that copies the pointer.
  typedef std::shared_ptr<folly::dynamic> DataPtr;

public:

  TaskStatus() : TaskStatus(TaskStatusBits::Empty) {}
  TaskStatus(const TaskStatus&) = default;
  TaskStatus(TaskStatus&&) noexcept = default;
  TaskStatus& operator=(TaskStatus&&) = default;
  TaskStatus& operator=(const TaskStatus&) = delete;

  ///
  /// Factory methods. Use these instead of the constructor to avoid setting
  /// the bits incorrectly.
  ///

  static TaskStatus running();
  static TaskStatus running(DataPtr&&);

  static TaskStatus done();
  static TaskStatus done(time_t timestamp);
  static TaskStatus done(DataPtr&&);

  static TaskStatus incomplete(DataPtr&&);

  static TaskStatus incompleteBackoff(DataPtr&&);

  static TaskStatus errorBackoff(DataPtr&&);
  static TaskStatus errorBackoff(const std::string& msg);

  // Only for notifyIfTasksNotRunning: overwriteable error that does not use
  // or advance backoff.
  static TaskStatus wasNotRunning();

  static TaskStatus failed();
  static TaskStatus failed(DataPtr&&);

  /**
   * Used by the scheduler when it finds that it never even tried to start
   * the task (a non-application failure).  Does not use backoff, does not
   * advance backoff.
   */
  static TaskStatus neverStarted(const std::string& msg);

  static TaskStatus workerLost(
    std::string worker_shard, uint32_t saved_backoff
  );

  /**
   * Parses a JSON status, or a simple string. When parsing JSON, "time" and
   * many other fields that are auto-populated by Bistro will be ignored.
   */
  static TaskStatus fromString(const std::string&) noexcept;

  ///
  /// Mutators
  ///

  // To be used **only** with scheduler- or worker-generated statuses.
  void markOverwriteable() {
    bits_ = bits_ | TaskStatusBits::Overwriteable;
  }

  /**
   * Update a status in-place. Use nextBackoffDuration from the RunningTask
   * if UseBackoff is set, but DoesNotAdvanceBackoff is not.
   */
  const TaskStatus& update(
      const cpp2::RunningTask& rt,
      TaskStatus&& new_status);

  /**
   * Replace Failed with Error. Clear backoff duration if it's in use.
   *
   * If the status has a saved backoff (aka came from a "workerLost()"), we
   * will (silently) leave the backoff duration alone.  Silence isn't the
   * best policy, and returning a meaningful message would be better, but
   * the odds that somebody will notice that "forgiving a lost worker task"
   * is not instantaneous are pretty low.
   */
  void forgive();

  ///
  /// Bit tests
  ///

  bool isEmpty() const {
    return bits_ == TaskStatusBits::Empty;
  }

  /**
   * CAREFUL: getPtr()->isRunning() does not know about the running tasks
   * belonging to deleted jobs.  This function only exists so that the main
   * scheduling loop can efficiently ignore running tasks.
   *
   * To correctly enumerate all running tasks, use {copy,get}RunningTasks.
   */
  bool isRunning() const {
    return allSet(bits_, TaskStatusBits::Running);
  }

  bool isDone() const {
    return allSet(bits_, TaskStatusBits::Done);
  }

  bool isFailed() const {
    return allSet(bits_, TaskStatusBits::Failed);
  }

  // TaskStatus does not know `noMoreBackoffs`, so it is not checked.
  bool isInBackoff(time_t cur_time) const {
    // kBistroSavedBackoff is irrelevant for the deciding whether we are in
    // backoff -- it is only used for computing the next one.
    return usesBackoff() && (timestamp_ + backoffDuration_) > cur_time;
  }

  bool isOverwriteable() const {
    return allSet(bits_, TaskStatusBits::Overwriteable);
  }

  ///
  /// Other accessors
  ///

  time_t timestamp() const {
    return timestamp_;
  }

  /**
   * IMPORTANT: This is NOT the effective backoff duration, which can be
   * longer (this is required for workerLost() to be safe). Instead, this
   * returns the backoff that would have been configured for the job, so
   * that we can correctly compute the next backoff value in getNext().
   */
  cpp2::BackoffDuration configuredBackoffDuration() const;

  // Not thread-safe. Only OK to call on copies of the TaskStatus that are
  // not at risk of being mutated concurrently.
  const folly::dynamic* dataThreadUnsafe() const {
    return data_.get();
  }

  TaskStatusBits bits() const {
    return bits_;
  }

  // Saves bits_ and data_, but not timestamp_, as fromString ignores "time"
  folly::dynamic toDynamicNoTime() const;

  // Captures bits_, timestamp_, and data_
  folly::dynamic toDynamic() const;

  // folly::toJson(toDynamic())
  std::string toJson() const;

private:
  explicit TaskStatus(TaskStatusBits bits, time_t timestamp)
    : timestamp_(timestamp),
      // Strictly speaking, we only need to initialize this for 'error'
      // statuses -- if you like, change that and see if it's faster.
      backoffDuration_(0),
      bits_(bits) {}

  explicit TaskStatus(TaskStatusBits bits)
    : TaskStatus(bits, time(nullptr)) {}

  explicit TaskStatus(TaskStatusBits bits, DataPtr&& d)
      : TaskStatus(bits) {
    data_ = std::move(d);
  }

  bool usesBackoff() const {
    return allSet(bits_, TaskStatusBits::UsesBackoff);
  }

  bool hasSavedBackoff() const {
    return allSet(bits_, TaskStatusBits::HasSavedBackoff);
  }

  // Ordered for space efficiency: 16-byte, 8-byte, 4-byte, 2-byte => 32 bytes
  //
  // TODO: Since different fields are used for different statuses, a union
  // type via boost::variant would provide greater type safety.
  DataPtr data_;  // For running statuses, only TaskStatusObservers use this
  time_t timestamp_;  // For running statuses, use RunningTask's startTime
  // The effective backoff. If HasSavedBackoff is not set, this also
  // determines the next backoff, otherwise kBistroSavedBackoff does.
  uint32_t backoffDuration_;  // Used only for InBackoff statuses
  TaskStatusBits bits_;
};

}}
