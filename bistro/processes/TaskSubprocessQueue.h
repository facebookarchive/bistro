/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <folly/MPMCQueue.h>
#include <folly/Synchronized.h>
#include <folly/Subprocess.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <folly/executors/IOThreadPoolExecutor.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/physical/AllTasksPhysicalResourceMonitor.h"
#include "bistro/bistro/physical/TaskPhysicalResourceFetcher.h"
#include "bistro/bistro/processes/AsyncReadPipeRateLimiter.h"

namespace folly { class dynamic; }

namespace facebook { namespace bistro {

class BaseLogWriter;
class TaskStatus;
namespace detail { class TaskSubprocessState; }

/**
 * Captures all the Bistro-specific logic of starting, monitoring, and
 * killing task subprocesses.
 *
 * 'final' because its constructor spawns threads, and derived classes would
 * likely be broken and racy.
 */
class TaskSubprocessQueue final {
private:
  // startTime, rand, job, node
  using InvocationID = std::tuple<int64_t, int64_t, std::string, std::string>;

  static InvocationID makeInvocationID(const cpp2::RunningTask& rt) {
    return InvocationID(
      rt.invocationID.startTime, rt.invocationID.rand, rt.job, rt.node
    );
  }

public:
  using StatusCob =
    std::function<void(const cpp2::RunningTask&, TaskStatus&&)>;
  using ResourceCob = std::function<
    void(const cpp2::RunningTask&, cpp2::TaskPhysicalResources&&)
  >;

  explicit TaskSubprocessQueue(std::unique_ptr<BaseLogWriter>);
  ~TaskSubprocessQueue();  // Sleeps until all tasks exit.

  // NOT movable or copiable since tasks_ referenced from a callback
  TaskSubprocessQueue(TaskSubprocessQueue&&) = delete;
  TaskSubprocessQueue& operator=(TaskSubprocessQueue&&) = delete;
  TaskSubprocessQueue(const TaskSubprocessQueue&) = delete;
  TaskSubprocessQueue& operator=(const TaskSubprocessQueue&) = delete;

  /**
   * Enqueues the task to be started on, and supervised by one of the
   * internal EventBase threads.  status_cob will be called from that thread
   * when the task exits, or if it fails to start.  Throws if this task
   * invocation is already running.
   */
  void runTask(
    const cpp2::RunningTask& rt,
    const std::vector<std::string>& cmd,  // Executable plus leading arguments
    const std::string& job_arg,  // The final, JSON argument for the task
    const boost::filesystem::path& working_dir,  // Start the task here
    // MUST be noexcept and thread-safe; called from an EventBase thread.
    StatusCob status_cob,
    // MUST be noexcept and thread-safe; called from an EventBase thread.
    // Runs periodically, only while the process is running.  Might never
    // get called for short tasks.
    ResourceCob resource_cob,
    cpp2::TaskSubprocessOptions opts
  );

  /**
   * Sends a signal to the task in the hopes of making it quit. Does not
   * wait for the task to exit.
   *
   * Throws if the signal could not be sent. This happens if such a task is
   * not running -- note that the entire invocation ID must match (we won't
   * accidentally kill a newer instance of a task), or if it got signals
   * more rapidly than it can handle.
   */
  void kill(const cpp2::RunningTask&, cpp2::KillRequest);

  /**
   * Racy: A task can exit even as we return `true`. Useful only for
   * confirming that task that was previously known to be runninig has
   * exited (assuming invocation IDs are not recycled).
   */
  bool isRunning(const cpp2::RunningTask& rt) const;

  const BaseLogWriter* getLogWriter() const { return logWriter_.get(); }

  int statusPipeFdForTest() const { return childStatusPipePlaceholder_.fd(); }

private:
  void logEvent(
    int glog_level,
    const cpp2::RunningTask&,
    const detail::TaskSubprocessState*,  // non-null prints rawStatus_
    const std::string& event,
    folly::dynamic&&  // The log object, fields will be added
  ) noexcept;

  void waitForSubprocessAndPipes(
    const cpp2::RunningTask& rt,
    // Mutated: acquires a pipe rate limiter, used
    std::shared_ptr<detail::TaskSubprocessState> state,
    folly::Subprocess&& proc,
    TaskSubprocessQueue::StatusCob status_cob
  ) noexcept;

  std::unique_ptr<BaseLogWriter> logWriter_;  // Must outlive all tasks.
  folly::File childStatusPipePlaceholder_;  // Reserved for child status pipes.
  folly::File childCanaryPipePlaceholder_;  // Helps wait for grandchildren, &c
  folly::Synchronized<std::map<
    InvocationID, std::shared_ptr<detail::TaskSubprocessState>
  >> tasks_;
  // Task states keep a pointer to this monitor, so declare it after.
  AllTasksPhysicalResourceMonitor tasksResourceMonitor_;
  // Owns the threads that actually use the tasks. Declared and constructed
  // last to avoid several common threading issues:
  //  - constructor threw after spawning threads => abort() or leak threads
  //  - threads using partially-constructed object => heisenbugs
  //  - threads using partially-destructed members => heisenbugs
  folly::IOThreadPoolExecutor threadPool_;
};

namespace detail {
/**
 * A Bistro task subprocess is a loosely-bound constellation of an
 * AsyncSubprocess and a few AsyncReadPipes -- all self-owned objects
 * residing on the **same** EventBase -- as well as TaskSubprocessQueue
 * callbacks attached via futures.  Many of these callbacks hold a
 * shared_ptr<TaskSubprocessState>.
 *
 * This state class helps implement a signal with "SIGTERM-wait-SIGKILL"
 * soft-kill logic, and log line rate limiting.
 */
class TaskSubprocessState {
public:
  //
  // Unless otherwise noted, NOTHING below is thread-safe, everything may
  // only be used from the EventBase thread of the AsyncSubprocess.
  //

  TaskSubprocessState(
    const cpp2::RunningTask&,
    cpp2::TaskSubprocessOptions,
    AllTasksPhysicalResourceMonitor*,
    TaskSubprocessQueue::ResourceCob&&
  );
  void asyncSubprocessCallback(const cpp2::RunningTask& rt,
                               folly::Subprocess& proc) noexcept;

  // Returns true after the signal was actually sent.
  bool wasKilled() const { return wasKilled_; }

  // These are used only by TaskSubprocessQueue, stored here for convenience.
  std::unique_ptr<AsyncReadPipeRateLimiter> pipeRateLimiter_;  // Created late
  std::string rawStatus_;

  /**
   * Thread-safe, called via TaskSubprocessQueue::kill(). Asks the next
   * invocation of asyncSubprocessCallback() to kill the subprocess.  Pass 0
   * for "SIGKILL now".  A positive `after_ticks` sends a SIGTERM now, and
   * schedules a SIGKILL after the given number of callback invocations.
   * Multiple requests received by a single asyncSubprocessCallback() call
   * coalesce into a single signal; if both TERM and KILL were requested,
   * only KILL is sent.  Only one KILL signal can be scheduled at any time
   * -- with multiple requests, the earlier KILL wins.  An immediate KILL
   * cancels any future scheduled KILLs.
   *
   * Throws if the signal could not be sent. This happens if such a task is
   * not running, or if it got signals more rapidly than it can handle.
   */
  void kill(cpp2::KillRequest r);

  const cpp2::TaskSubprocessOptions& opts() const { return opts_; }
  const std::string& cgroupName() const { return cgroupName_; }

private:
  int64_t getNumPolls() const {
    // resourceCallback_ will be called from asyncSubprocessCallback(),
    // which is executed every opts_.pollMs on the EventBase thread.  To
    // This computes the number of asyncSubprocessCallback() invocations per
    // one resourceCallback_ invocation.
    // Convert resource callback invocation interval to milliseconds
    const double resInterval = std::max(1, opts_.refreshResourcesSec) * 1000.;
    const double pollInterval = std::max(1, opts_.pollMs);
    // Round up the ratio to make sure resource callback invocation happens
    // no more frequently than requested.
    return std::ceil(resInterval / pollInterval);
  }

  const cpp2::TaskSubprocessOptions opts_;
  folly::MPMCQueue<cpp2::KillRequest> queue_;
  uint32_t killAfterTicks_{0};  // 0 says 'do not kill'; timer for operator()
  bool wasKilled_{false};
  TaskSubprocessQueue::ResourceCob resourceCallback_;
  int64_t numPolls_{0};
  std::string cgroupName_;  // The last, per-task, part of the cgroup path.
  TaskPhysicalResourceFetcher physicalResourceFetcher_;
};
}  // namespace detail

}}  // namespace facebook::bistro
