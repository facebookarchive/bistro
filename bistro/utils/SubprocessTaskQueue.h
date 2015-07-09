/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <boost/filesystem/path.hpp>
#include <folly/MPMCQueue.h>
#include <folly/Synchronized.h>
#include <memory>
#include <string>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/utils/BackgroundThreadMixin.h"
#include "bistro/bistro/utils/ProcessRunner.h"
#include "bistro/bistro/utils/TaskIDMap.h"
#include <folly/experimental/AutoTimer.h>

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro {

class LogWriter;
class TaskStatus;

/**
 * Supervises concurrent subprocesses using a number of worker threads.
 *
 * DO: The worker thread count could be drastically reduced by polling
 * multiple tasks from one thread...  but there's no present need.
 */
class SubprocessTaskQueue : public BackgroundThreadMixin {
private:
  struct Task;

  typedef std::function<void(
    const cpp2::RunningTask& rt,
    TaskStatus&& status
  )> TaskCallback;

public:
  SubprocessTaskQueue(
    const boost::filesystem::path& log_db_file,
    const boost::filesystem::path& pipe_dir
  );
  ~SubprocessTaskQueue() override;

  LogWriter* getLogWriter() { return logWriter_.get(); }

  /**
   * Starts the task.  If the task fails to start, will invoke the callback
   * and exit.  Otherwise, enqueues it to be supervised by a worker thread.
   */
  void runTask(
    const cpp2::RunningTask& rt,
    const std::vector<std::string>& cmd,  // Executable plus leading arguments
    const std::string& job_arg,  // The final, JSON argument for the task
    const boost::filesystem::path& wd,  // Start the task in this directory
    TaskCallback cb  // Must be noexcept; called once on exit / kill.
  ) noexcept;

  /**
   * Soft-kills or hard-kills the task, and invokes its callback with its
   * status, which defaults to BACKOFF (if e.g. SIGTERM wasn't handled),
   * subject to the status filter.  Does nothing for tasks that already
   * finished, were enqueued but not started, or were never even enqueued
   * via runTask.  Thread-safe.
   */
  void killTask(
    const std::string& job,
    const std::string& node,
    cpp2::KillMethod kill_method,  // soft (TERM-wait-KILL) or hard (KILL)
    cpp2::KilledTaskStatusFilter status_filter  // Explained in .thrift file
  );

private:
  // A thin wrapper around ProcessRunner
  struct Task {
    Task(
      const cpp2::RunningTask& rt,
      const std::vector<std::string>& cmd,
      const std::string& job_arg,
      const boost::filesystem::path& wd,
      TaskCallback cb,
      SubprocessTaskQueue* queue
    );

    // Doesn't need synchronization since it's an immutable struct.
    const cpp2::RunningTask runningTask_;

    // These are synchronized implicitly, see the code.
    folly::AutoTimer<> timer_;  // Logs how long the task ran
    const TaskCallback callback_;

    // Must be initialized last, since initialization starts the subprocess.
    // Has its own internal synchronization for *Kill() / wait().
    ProcessRunner runner_;
  };

  friend class Task;  // It accesses pipeDir_, logWriter_, etc.

  std::chrono::milliseconds work() noexcept;
  void kill(
    std::shared_ptr<Task> task,
    cpp2::KillMethod kill_method,  // soft (TERM-wait-KILL) or hard (KILL)
    cpp2::KilledTaskStatusFilter status_filter  // Explained in .thrift file
  );

  boost::filesystem::path pipeDir_;
  std::unique_ptr<LogWriter> logWriter_;
  folly::MPMCQueue<std::shared_ptr<Task>> taskQueue_;
  // This map enables us to find running tasks so we can kill them.
  folly::Synchronized<TaskIDMap<std::shared_ptr<Task>>> idToTask_;
};

}}
