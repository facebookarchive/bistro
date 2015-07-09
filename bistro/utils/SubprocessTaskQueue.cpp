/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/SubprocessTaskQueue.h"

#include <boost/filesystem.hpp>
#include <folly/json.h>
#include <folly/Random.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/utils/LogWriter.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"

DEFINE_int32(
  worker_threads, 100,
  "Number of worker threads, i.e. the maximum number of concurrent child "
  "processes that we can monitor"
);

namespace facebook { namespace bistro {

using apache::thrift::debugString;
using namespace folly;
using namespace std;

namespace {
  // Together with kPollPeriodMs from KillableSubprocess.cpp determines the
  // minimum latency of a task.
  const int kMeanIdleWaitMs = 20;
}

SubprocessTaskQueue::SubprocessTaskQueue(
    const boost::filesystem::path& log_db_file,
    const boost::filesystem::path& pipe_dir)
  : pipeDir_(pipe_dir),
    logWriter_(new LogWriter(log_db_file)),
    taskQueue_(100000) {

  boost::filesystem::create_directories(pipeDir_);
  for (int i = 0; i < FLAGS_worker_threads; ++i) {
    runInBackgroundLoop(bind(&SubprocessTaskQueue::work, this));
  }
}

SubprocessTaskQueue::~SubprocessTaskQueue() {
  stopBackgroundThreads();
}

void SubprocessTaskQueue::runTask(
    const cpp2::RunningTask& rt,
    const vector<string>& cmd,
    const string& job_arg,
    const boost::filesystem::path& wd,
    TaskCallback cb) noexcept {

  SYNCHRONIZED(idToTask_) {
    // Start the task while idToTask_ is locked to avoid racing *Kill()
    try {
      auto task = make_shared<Task>(rt, cmd, job_arg, wd, cb, this);
      CHECK(idToTask_.insert(rt.job, rt.node, task))
        << "Task was already running: " << debugString(rt);
      // The task already started; there's no going back if the queue is full.
      taskQueue_.blockingWrite(task);
    } catch (const exception& e) {
      auto msg =  // ProcessRunner errors include the command, don't add it
        folly::to<string>("Error starting task on ", rt.node, ": ", e.what());
      LOG(ERROR) << msg;
      cb(rt, TaskStatus::errorBackoff(msg));  // May throw
      return;
    }
  }
}

SubprocessTaskQueue::Task::Task(
    const cpp2::RunningTask& rt,
    const vector<string>& cmd,
    const string& job_arg,
    const boost::filesystem::path& wd,
    TaskCallback cb,
    SubprocessTaskQueue* queue)
  : runningTask_(rt),
    callback_(cb),
    runner_(
      [this, queue](LogTable lt, StringPiece s) {
        queue->logWriter_->write(
          lt,
          runningTask_.job,
          runningTask_.node,
          s
        );
      },
      wd,  // The task will chdir() into this working dir after fork()
      cmd,
      // Initialize the pipe and arguments together
      [rt, job_arg, queue]() {
        auto pipe_file = make_unique<TemporaryFile>(queue->pipeDir_);
        auto pipe_filename = pipe_file->getFilename().native();
        return make_pair(
          vector<string>{
            rt.node,
            pipe_filename,  // temporary file to capture task status
            job_arg  // JSON data blob
          },
          std::move(pipe_file)
        );
      }()
    ) {
}

namespace {  // Helpers for kill() & work()

Optional<string> killViaMethod(ProcessRunner& runner, cpp2::KillMethod m) {
  switch (m) {
    case cpp2::KillMethod::SOFT:
      return runner.softKill();
    case cpp2::KillMethod::HARD:
      return runner.hardKill();
    default:
      LOG(ERROR) << "Ignoring invalid kill method: " << static_cast<int>(m);
      return none;
  }
}

TaskStatus filterStatusForceDoneOr(
    cpp2::KilledTaskStatusFilter filter,
    TaskStatus status) {
  switch (filter) {
    case cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_FAILED:
      return TaskStatus::failed(folly::make_unique<folly::dynamic>(
        folly::dynamic::object
        ("message", "Killed & coerced to 'failed' by since task was not done")
        ("actual_status", status.toDynamicNoTime())
      ));
    case cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE_BACKOFF:
      return TaskStatus::incompleteBackoff(folly::make_unique<folly::dynamic>(
        folly::dynamic::object
        ("message",
         "Killed & coerced to 'incomplete_backoff' since task was not done")
        ("actual_status", status.toDynamicNoTime())
      ));
    case cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE:
      return TaskStatus::incomplete(folly::make_unique<folly::dynamic>(
        folly::dynamic::object
        ("message", "Killed & coerced to 'incomplete' since task was not done")
        ("actual_status", status.toDynamicNoTime())
      ));
    default:
      ;  // Fall through
  }
  // Abort since we already pre-filtered the status!
  LOG(FATAL) << "Bad status filter:" << static_cast<int>(filter);
  return std::move(status);  // Not reached.
}

TaskStatus filterStatus(
    cpp2::KilledTaskStatusFilter filter,
    TaskStatus status) {
  switch (filter) {
    case cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_FAILED:
    case cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE_BACKOFF:
    case cpp2::KilledTaskStatusFilter::FORCE_DONE_OR_INCOMPLETE:
      if (status.isDone()) {
        break;
      }
      return filterStatusForceDoneOr(filter, std::move(status));
    case cpp2::KilledTaskStatusFilter::NONE:
      break;
    default:  // Maybe this worker doesn't support the new filter type?
      LOG(ERROR) << "Unknown status filter: " << static_cast<int>(filter);
  }
  return std::move(status);
}

}  // anonymous namespace

void SubprocessTaskQueue::kill(
    shared_ptr<Task> task,
    cpp2::KillMethod kill_method,
    cpp2::KilledTaskStatusFilter status_filter) {

  // This is a no-op if the ProcessRunner has already exited.  E.g. we would
  // get 'none' here if kill() runs between wait() and idToTask_.erase() in
  // work(), or if the client calls kill() twice, etc.
  auto maybe_raw_status = killViaMethod(task->runner_, kill_method);
  if (!maybe_raw_status.hasValue()) {
    return;
  }
  auto raw_status = maybe_raw_status.value();
  // Getting this far is mutually exclusive with wait() returning a value.
  // Thanks to "if (!was_killed)" below, callback_ & timer_ don't need locks.

  // Parse the task's status string.
  auto status = filterStatus(
    status_filter,
    raw_status.empty()
      // Task backs off, but its backoff counter is not advanced. Good for
      // preempting or restarting running tasks for whatever reason.
      ? TaskStatus::incompleteBackoff(folly::make_unique<folly::dynamic>(
          dynamic::object("exception", "Task killed, no status returned")
        ))
      : TaskStatus::fromString(raw_status)
  );

  // Log the runtime, and the post-filter task status
  task->timer_.log(
    "Task ", debugString(task->runningTask_), " killed with status '",
    status.toJson(), "' (raw text '", raw_status, "')"
  );

  // If this throws, we crash hard. That's by design.
  task->callback_(task->runningTask_, std::move(status));
}

void SubprocessTaskQueue::killTask(
    const string& job,
    const string& node,
    cpp2::KillMethod kill_method,
    cpp2::KilledTaskStatusFilter status_filter) {

  auto maybe_task = idToTask_->get(job, node);
  if (!maybe_task.hasValue()) {
    return;  // A task might be unknown because it finished, or never ran.
  }
  // The task will be deleted from this map by work().

  kill(*maybe_task, kill_method, status_filter);
}

chrono::milliseconds SubprocessTaskQueue::work() noexcept {
  shared_ptr<Task> task;
  if (!taskQueue_.read(task)) {
    // This affects the latency with which we notice the completion of very
    // short-running tasks.  Only one thread needs to wake up out of many,
    // so the wait grows linearly in the number of threads.  Randomize the
    // delay so that the threads don't wake up in lockstep.  Note that this
    // latency is in addition to kPollPeriodMs from KillableSubprocess.cpp.
    int thread_mean_wait_ms = kMeanIdleWaitMs * FLAGS_worker_threads;
    return chrono::milliseconds(folly::Random::rand32(
      0.5 * thread_mean_wait_ms, 1.5 * thread_mean_wait_ms
    ));
  }

  // These two values represent the task status in case of error.
  string raw_status = "[COULD NOT READ]";
  auto status = TaskStatus::errorBackoff("Failed to read a status");
  bool was_killed = false;  // If true, skip the callback

  try {
    auto maybe_raw_status = task->runner_.wait();
    was_killed = !maybe_raw_status.hasValue();
    if (!was_killed) {
      // Parse the raw status string
      raw_status = maybe_raw_status.value();
      if (!raw_status.empty()) {
        status = TaskStatus::fromString(raw_status);
      }
    }
  } catch (const exception& e) {
    // ProcessRunner errors tend to include the command, so don't print it.
    auto msg = folly::to<string>(
      "Error running task on ", task->runningTask_.node, ": ", e.what()
    );
    LOG(ERROR) << msg;
    status = TaskStatus::errorBackoff(msg);
  }

  CHECK(idToTask_->erase(task->runningTask_.job, task->runningTask_.node) > 0)
    << "Task stopped twice: " << debugString(task->runningTask_);

  if (!was_killed) {
    // Getting this far is mutually exclusive with kill() returning a value,
    // so callback_ & timer_ don't need locks.
    if (task->runningTask_.job == "__BISTRO_HEALTH_CHECK__") {
      task->timer_.log(
        "Healthcheck started at ", task->runningTask_.invocationID.startTime,
        " quit with status '", status.toJson(), "'"
      );
    } else {
      task->timer_.log(
        "Task ", debugString(task->runningTask_), " quit with status '",
        status.toJson(), "' (raw text '", raw_status, "')"
      );
    }
    // If this throws, we crash hard. That's by design.
    task->callback_(task->runningTask_, std::move(status));
  }

  return chrono::seconds(0);
}

}}
