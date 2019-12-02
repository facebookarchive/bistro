/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/processes/TaskSubprocessQueue.h"

#include <boost/filesystem.hpp>
#include <folly/GLog.h>
#include <folly/Subprocess.h>
#include <folly/json.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"
#include "bistro/bistro/processes/AsyncCGroupReaper.h"
#include "bistro/bistro/processes/AsyncSubprocess.h"
#include "bistro/bistro/processes/CGroupSetup.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/LogWriter.h"
#include "bistro/bistro/utils/shell.h"

DEFINE_int32(
  task_thread_pool_size, 10,
  "How many threads to start to monitor task subprocesses. A high value "
  "wastes RAM and CPU, while a low value increases the latency with which "
  "Bistro processes task subprocess events."
);

DEFINE_int32(
  refresh_all_tasks_physical_resources_sec, 60,
  "Some resources (like GPUs) cannot be efficiently queried per task, and "
  "are much better served via batch queries to populate an in-memory cache. "
  "How often should these resources be refreshed?"
);

DEFINE_int32(
  physical_resources_subprocess_timeout_ms, 1000,
  "For some resources, we poll external an program (e.g. 'nvidia-smi'). How "
  "long should we wait before killing it and assuming resources are "
  "unavailable?"
);

namespace facebook { namespace bistro {

namespace {

// DO: Avoid duplication of this constant with RemoteWorker.h
const std::string kHealthcheckTaskJob = "__BISTRO_HEALTH_CHECK__";

// Reserve an FD so that we can safely use the same one for opening a
// pipe to a child.  This FD below is never used, it's always clobbered.
int makePlaceholderFd() {
  int pipes[2];
  folly::checkUnixError(pipe(pipes));
  folly::checkUnixError(close(pipes[1]));
  return pipes[0];
}

TaskStatus parseStatus(const detail::TaskSubprocessState* state) {
  if (state->rawStatus_.empty()) {
    if (state->wasKilled()) {
      // Task backs off, but its backoff counter is not advanced. Good
      // for preempting or restarting running tasks.
      return TaskStatus::incompleteBackoff(std::make_unique<folly::dynamic>(
        folly::dynamic::object
          ("exception", "Task killed, no status returned")
      ));
    } else {
      return TaskStatus::errorBackoff("Failed to read a status");
    }
  } else {
    return TaskStatus::fromString(state->rawStatus_);
  }
}

}  // anonymous namespace

void TaskSubprocessQueue::logEvent(
    int glog_level,
    const cpp2::RunningTask& rt,
    const detail::TaskSubprocessState* state,
    const std::string& event,
    folly::dynamic&& obj = folly::dynamic::object()) noexcept {

  obj["event"] = event;
  // Adding fields can only fail in case of programmer error, so add them
  // outside the try-catch.
  CHECK(state) << "state cannot be null";
  if (!state->rawStatus_.empty()) {
    // In theory, this can change -- StatusCob only gets the last line.
    obj["raw_status"] = state->rawStatus_;
  }
  // Distinguish different invocations of the same task.
  obj["invocation_start_time"] = rt.invocationID.startTime;
  obj["invocation_rand"] = rt.invocationID.rand;
  // The worker host need not be the same as the scheduler host, so
  // it's important to log it.
  obj["worker_host"] = getLocalHostName();
  // DO: If useful, also log rt.workerShard, or rt.nextBackoffDuration, or
  // even rt.nodeResources.

  // Contract: We always write a JSON message (serialization shouldn't fail).
  std::string msg = folly::toJson(obj);

  try {
    // Suppress INFO messages about healthchecks to keep the logs cleaner
    if (glog_level != google::INFO || rt.job != kHealthcheckTaskJob) {
      google::LogMessage(__FILE__, __LINE__, glog_level).stream()
        << "Task " << rt.job << ", " << rt.node << " message: " << msg;
    }
    logWriter_->write(LogTable::EVENTS, rt.job, rt.node, msg);
  } catch (const std::exception& e) {  // Not much to do with this...
    LOG(ERROR) << "Error logging task subprocess message '" << msg
      << "' for task " << apache::thrift::debugString(rt) << ": " << e.what();
  }
}

namespace {
uint32_t pollMs(const cpp2::TaskSubprocessOptions& opts) {
  return std::max(1, opts.pollMs);
}
}  // anonymous namespace

// Set up EventBased handlers to interact with the subprocess and its pipes.
// Once the process exits, and the pipes close, the final callback removes
// `state` from `tasks_`.
void TaskSubprocessQueue::waitForSubprocessAndPipes(
    const cpp2::RunningTask& rt,
    // Mutated: we add a per-task log line rate-limiter
    std::shared_ptr<detail::TaskSubprocessState> state,
    folly::Subprocess&& proc,
    TaskSubprocessQueue::StatusCob status_cob) noexcept {
    // Below, the `noexcept` guarantees that we don't delete from tasks_ twice

  // The pipe & subprocess handlers must run in the same EventBase thread,
  // since they mutate the same state.  Also, the rate-limiter
  // initialization below depends on `evb` running the present function.
  auto evb = folly::EventBaseManager::get()->getExistingEventBase();
  CHECK(evb);  // The thread pool should have made the EventBase.

  // Take ownership of proc's pipes, and set up their log processing on the
  // EventBase.  Produce 'pipe closed' futures.
  //
  // Technically, this block could be outside of the `noexcept`, but it
  // should not throw anyhow, so leave it in the same function.
  std::vector<folly::Future<folly::Unit>> pipe_closed_futures;
  std::vector<std::shared_ptr<AsyncReadPipe>> pipes;  // For the rate-limiter
  for (auto&& p : proc.takeOwnershipOfPipes()) {
    int child_fd = p.childFd;
    auto job = rt.job;
    auto node = rt.node;
    pipes.emplace_back(asyncReadPipe(
      evb,
      std::move(p.pipe),
      readPipeLinesCallback([
        this, job, node, child_fd, state
      ](AsyncReadPipe*, folly::StringPiece s) {
        if (s.empty()) {
          // Skip the empty line at the end of \n-terminated files
          return true;
        }

        // Enforce log line rate limits per task, rather than per job, since
        // it's cleaner.  For hacky per-job limits, treat log throughput as
        // a statically-allocated job resource.
        auto* rate_lim = state->pipeRateLimiter_.get();
        // This cannot fire, since these AsyncReadPipes are being created
        // from the same EventBase that will run them, and therefore
        // state->pipeRateLimiter_ will be set first, below.
        CHECK(rate_lim);
        if (child_fd == STDERR_FILENO) {
          logWriter_->write(LogTable::STDERR, job, node, s);
          rate_lim->reduceQuotaBy(1);
        } else if (child_fd == STDOUT_FILENO) {
          logWriter_->write(LogTable::STDOUT, job, node, s);
          rate_lim->reduceQuotaBy(1);
        } else if (child_fd == childStatusPipePlaceholder_.fd()) {
          // Discard delimiter since status parsing assumes it's gone.
          s.removeSuffix("\n");
          // Only use the last status line, no disk IO -- no line quota
          state->rawStatus_.assign(s.data(), s.size());
        }
        // Else: we don't care about other FDs -- in fact, the only other FD
        // it could reasonably be is the canary pipe, and we won't indulge
        // the children by paying attention to the junk they write there.
        return true;  // Keep reading
      },
      65000)  // Limit line length to protect the log DB
    ));
    pipe_closed_futures.emplace_back(pipes.back()->pipeClosed());
  }
  // Gets to run before any line callbacks, since the current scope was
  // running on the callbacks' EventBase before the callbacks even existed.
  state->pipeRateLimiter_.reset(new AsyncReadPipeRateLimiter(
    evb,
    pollMs(state->opts()),  // Poll at the Subprocess's rate
    state->opts().maxLogLinesPerPollInterval,
    std::move(pipes)
  ));

  // Add callbacks to wait for the subprocess and pipes on the same EventBase.
  collectAllSemiFuture(
      // 1) The moral equivalent of waitpid(), followed by wait for cgroup.
      asyncSubprocess( // Never yields an exception
          evb,
          std::move(proc),
          // Unlike std::bind, explicitly keeps the state alive.
          [rt, state](folly::Subprocess& p) {
            state->asyncSubprocessCallback(rt, p);
          },
          pollMs(state->opts()))
          .thenValue([ this, rt, state, evb ](
              folly::ProcessReturnCode && rc) noexcept {
            logEvent(
                google::INFO,
                rt,
                state.get(),
                "process_exited",
                folly::dynamic::object("message", rc.str())); // noexcept
            // Now that the child has exited, optionally use cgroups to kill,
            // and/or wait for, its left-over descendant processes.
            if (state->opts().cgroupOptions.subsystems.empty()) {
              // No cgroups exist: shortcut early to avoid logging
              // "cgroups_reaped".
              return folly::Future<folly::Unit>();
            }
            // Usually, well-behaved descendants will have exited by this point.
            // This reaper will write logspam, and wait until all processes in
            // each of the task's cgroups have exited.  If the `freezer`
            // subsystem is available, or if `killWithoutFreezer` is set, the
            // reaper will also repeatedly send them SIGKILL.
            //
            // This future fires only when the task cgroups lose all their
            // tasks.
            return asyncCGroupReaper(
                       evb,
                       state->opts().cgroupOptions,
                       state->cgroupName(),
                       pollMs(state->opts()))
                .thenTry(
                    [ this, rt, state ](folly::Try<folly::Unit> t) noexcept {
                      t.throwIfFailed(); // The reaper never thows, crash if it
                                         // does.
                      logEvent(google::INFO, rt, state.get(), "cgroups_reaped");
                    });
          }),
      // 2) Wait for the child to close all pipes
      collectAllSemiFuture(pipe_closed_futures)
          .toUnsafeFuture()
          .thenValue([ this, rt, state ](
              std::vector<folly::Try<folly::Unit>> &&
              all_closed) noexcept { // Logs and swallows all exceptions
            for (auto& try_pipe_closed : all_closed) {
              try {
                // DO: Use folly::exception_wrapper once wangle supports it.
                try_pipe_closed.throwIfFailed();
              } catch (const std::exception& e) {
                // Carry on, the pipe is known to be closed. Logging is
                // noexcept.
                logEvent(
                    google::ERROR,
                    rt,
                    state.get(),
                    "task_pipe_error",
                    folly::dynamic::object("message", e.what())); // noexcept
              }
            }
            logEvent(
                google::INFO, rt, state.get(), "task_pipes_closed"); // noexcept
          }))
      .toUnsafeFuture()
      .thenValue([ this, rt, status_cob, state ](
          // Safe to ignore exceptions, since the above callbacks are noexcept.
          std::tuple<folly::Try<folly::Unit>, folly::Try<folly::Unit>> &&
          // `noexcept` since this is the final handler -- nothing inspects
          // its result.
          ) noexcept {
        // Parse the task's status string, if it produced one. Note that in
        // the absence of a status, our behavior is actually racy.  The task
        // might have spontaneously exited just before Bistro tried to kill
        // it.  In that case, wasKilled() would be true, and we would
        // "incorrectly" report incompleteBackoff().  This is an acceptable
        // tradeoff, since we *need* both sides of the wasKilled() branch:
        //  - We must decrement the retry counter on spontaneous exits
        //    with no status (e.g. C++ program segfaults).
        //  - We must *not* decrement the retry counter for killed tasks that
        //    do not output a status (i.e. no SIGTERM handler), since
        //    preemption should be maximally transparent.
        auto status = parseStatus(state.get());
        // Log and report the status
        logEvent(
            google::INFO,
            rt,
            state.get(),
            "got_status",
            // The NoTime variety omits "backoff_duration", but we lack it
            // anyway, since only TaskStatusSnapshot calls TaskStatus::update().
            folly::dynamic::object("status", status.toDynamicNoTime()));
        status_cob(rt, std::move(status)); // noexcept
        // logEvent ignores healthcheck messages; print a short note instead.
        if (rt.job == kHealthcheckTaskJob) {
          LOG(INFO) << "Healthcheck started at " << rt.invocationID.startTime
                    << " quit with status '" << status.toJson() << "'";
        }
        SYNCHRONIZED(
            tasks_) { // Remove the completed task's state from the map.
          if (tasks_.erase(makeInvocationID(rt)) == 0) {
            LOG(FATAL) << "Missing task: " << apache::thrift::debugString(rt);
          }
        }
      });
}

TaskSubprocessQueue::TaskSubprocessQueue(
  std::unique_ptr<BaseLogWriter> log_writer
) : logWriter_(std::move(log_writer)),
    childStatusPipePlaceholder_(makePlaceholderFd(), /*owns_fd=*/ true),
    childCanaryPipePlaceholder_(makePlaceholderFd(), /*owns_fd=*/ true),
    tasksResourceMonitor_(
      std::max(10, FLAGS_physical_resources_subprocess_timeout_ms),
      std::chrono::seconds(
        std::max(1, FLAGS_refresh_all_tasks_physical_resources_sec)
      )
    ),
    // See the header for the reasons this should be the *last* thing this
    // constructor does.
    threadPool_(FLAGS_task_thread_pool_size) {}

TaskSubprocessQueue::~TaskSubprocessQueue() {
  // Blithely wait for all tasks to exit.
  while (true) {
    auto num_tasks = tasks_.wlock()->size();
    if (num_tasks == 0) {
      break;
    }
    FB_LOG_EVERY_MS(INFO, 5000) << "Waiting for " << num_tasks << " tasks";
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void TaskSubprocessQueue::runTask(
    const cpp2::RunningTask& rt,
    const std::vector<std::string>& cmd,
    const std::string& job_arg,
    const boost::filesystem::path& working_dir,
    StatusCob status_cob,
    ResourceCob resource_cob,
    cpp2::TaskSubprocessOptions opts) {

  // Set up the task's map entry up-front, because it is a usage error to
  // start two tasks with the same invocation ID.  It also must be done
  // before setting up the callbacks, since those expect tasks_ to be ready,
  // and *may* run immediately.  Also, the destructor relies on tasks_ being
  // added to synchronously.
  auto state = std::make_shared<detail::TaskSubprocessState>(
    rt,
    std::move(opts),
    &tasksResourceMonitor_,  // Outlives task states
    std::move(resource_cob)
  );
  SYNCHRONIZED(tasks_) {
    if (!tasks_.emplace(makeInvocationID(rt), state).second) {
      throw BistroException(
        "Task already running: ", apache::thrift::debugString(rt)
      );
    }
  }

  // Passing 'this' is not too scary, since the EventBase threads are the
  // first to be stopped / destroyed on this object.
  threadPool_.add([this, rt, cmd, job_arg, working_dir, status_cob, state]() {
    std::string debug_cmd{"unknown"};
    try {
      // Populate `debug_cmd` first, since this shouldn't throw, and it's
      // required for nice exception logging.
      auto pipe_filename =  // Unlike /proc, /dev/fd works on Linux and OS X
        folly::to<std::string>("/dev/fd/", childStatusPipePlaceholder_.fd());
      std::vector<std::string> args{rt.node, pipe_filename, job_arg};
      debug_cmd = folly::to<std::string>(
        '[', escapeShellArgsInsecure(cmd), "] + [",
        escapeShellArgsInsecure(args), ']'
      );

      boost::system::error_code ec;
      if (!boost::filesystem::exists(working_dir, ec)) {
        boost::filesystem::create_directories(working_dir, ec);
      }
      if (ec) {
        throw BistroException(
            "Failed to make working directory: ", ec.message());
      }

      CHECK(cmd.size() >= 1);
      std::vector<std::string> full_cmd{cmd};
      full_cmd.insert(full_cmd.end(), args.begin(), args.end());
      logEvent(google::INFO, rt, state.get(), "running", folly::dynamic::object
        ("command", folly::dynamic(full_cmd.begin(), full_cmd.end()))
      );

      auto opts = folly::Subprocess::Options().pipeStdout().pipeStderr()
        .chdir(working_dir.native())
        .fd(childStatusPipePlaceholder_.fd(), folly::Subprocess::PIPE_OUT);
      if (state->opts().useCanaryPipe) {
        // It's much easier for us to get the read end of the pipe, since we
        // can just use AsyncReadPipe to track its closing.
        opts.fd(childCanaryPipePlaceholder_.fd(), folly::Subprocess::PIPE_OUT);
      }
      if (state->opts().parentDeathSignal != 0) {
        opts.parentDeathSignal(state->opts().parentDeathSignal);
      }
      if (state->opts().processGroupLeader) {
        opts.processGroupLeader();
      }
      // folly::Subprocess() can throw, and that's ok. Do not make `proc`
      // inline with waitForSubprocessAndPipes to ensure that `state` is
      // safe to use in the `catch` below.
      auto proc = [&state, &full_cmd](
        // Pass by r-value, since opts will contain an invalid pointer after
        // add_to_cgroups is destroyed below.
        folly::Subprocess::Options&& options
      ) {
        if (state->cgroupName().empty()) {
          return folly::Subprocess(full_cmd, options);
        }
        LOG(INFO) << "Making task cgroups named " << state->cgroupName();
        // This must live only until folly::Subprocess's constructor exits.
        AddChildToCGroups add_to_cgroups(cgroupSetup(
          state->cgroupName(), state->opts().cgroupOptions
        ));
        options.dangerousPostForkPreExecCallback(&add_to_cgroups);
        return folly::Subprocess(full_cmd, options);
      }(std::move(opts));
      // IMPORTANT: This call must be last in the block and `noexcept` to
      // ensure that the below "remove from tasks_" cannot race the "remove
      // from tasks_" in the "subprocess exited and pipes closed" callback.
      waitForSubprocessAndPipes(
        rt, std::move(state), std::move(proc), std::move(status_cob)
      );
    } catch (const std::exception& e) {
      SYNCHRONIZED(tasks_) {  // Remove the task -- its callback won't run.
        if (tasks_.erase(makeInvocationID(rt)) == 0) {
          LOG(FATAL) << "Missing task: " << apache::thrift::debugString(rt);
        }
      }
      auto msg = folly::to<std::string>(
        "Failed to start task ", apache::thrift::debugString(rt), " / ",
        debug_cmd, ": ", e.what()
      );
      auto status = TaskStatus::errorBackoff(msg);
      // Although `state` is moved above, this is ok, since our move
      // constructors and waitForSubprocessAndPipes must be noexcept.
      logEvent(
        google::ERROR, rt, state.get(), "task_failed_to_start",
        folly::dynamic::object("status", status.toDynamicNoTime())
      );  // noexcept
      // Advances the backoff/retry counter, so we eventually give up.
      status_cob(rt, std::move(status));  // noexcept
    }
  });
}

bool TaskSubprocessQueue::isRunning(const cpp2::RunningTask& rt) const {
  SYNCHRONIZED_CONST(tasks_) {
    if (tasks_.find(makeInvocationID(rt)) == tasks_.end()) {
      return false;
    }
  }
  return true;
}

void TaskSubprocessQueue::kill(
    const cpp2::RunningTask& rt,
    cpp2::KillRequest req) {
  SYNCHRONIZED(tasks_) {
    auto it = tasks_.find(makeInvocationID(rt));
    if (it == tasks_.end()) {
      // We don't attempt to kill tasks that have not started yet. Doing
      // that requires state, and the best place for this state is
      // Bistro's scheduler (see the "kill orphan tasks" code).
      throw BistroException(
        "Cannot kill task with ID ", rt.job, " / ", rt.node, " / ",
        rt.invocationID.startTime, " / ", rt.invocationID.rand,
        " since no such task is running."
      );
    }
    it->second->kill(std::move(req));  // Throws if the queue is full.
  }
}

namespace detail {

namespace {
std::string makeCGroupName(
    const cpp2::CGroupOptions& cgopts,
    const cpp2::RunningTask& rt) {
  if (cgopts.subsystems.empty()) {
    return std::string();
  }
  // Create a unique ID based on the RunningTask and the supervisor's pid.
  constexpr size_t kDateMax = 32; // 4+2+2+2+2+2+1 + 17 for sheer paranoia.
  struct ::tm start_tm;
  // Using localtime would cause DST headaches, and confusion in unit tests.
  CHECK(gmtime_r(&rt.invocationID.startTime, &start_tm));
  char start_time[kDateMax];
  CHECK_LT(0, strftime(start_time, kDateMax, "%Y%m%d%H%M%S", &start_tm));
  return folly::format(
    // Tasks are never hierarchical, because e.g. `memory` hierarchy support
    // is not cleanly composable, and `cpu` hierarchies also have interesting
    // side effects. It was tempting to make the workerShard be a sub-cgroup,
    // but it is a bad idea. Therefore, all tasks are flat.
    //
    // In fact, if you have multiple workers on your host, you should almost
    // certainly allocate different slices to them.  Therefore, the presence
    // of workerShard in the task ID is at best precautionary (avoiding
    // collisions between worker instances), and aspirational (we could use
    // it to have a worker re-adopt previously running tasks on startup).
    "{}:{}:{:x}:{}", rt.workerShard, start_time, rt.invocationID.rand, getpid()
  ).str();
}
}  // anonymous namespace

// Hardcoded constant: 10 signals per ~10ms seems plenty.
TaskSubprocessState::TaskSubprocessState(
  const cpp2::RunningTask& rt,
  cpp2::TaskSubprocessOptions opts,
  AllTasksPhysicalResourceMonitor* all_tasks_mon,
  TaskSubprocessQueue::ResourceCob&& resource_cb
) : opts_(std::move(opts)),
    queue_(10),
    resourceCallback_(std::move(resource_cb)),
    cgroupName_(makeCGroupName(opts_.cgroupOptions, rt)),
    physicalResourceFetcher_(
      CGroupPaths(opts_.cgroupOptions, boost::filesystem::path(cgroupName_)),
      all_tasks_mon
    ) {
}

void TaskSubprocessState::asyncSubprocessCallback(
    const cpp2::RunningTask& rt,
    folly::Subprocess& proc) noexcept {
  int signal = 0;  // No signal
  // Is it time to send a previously scheduled KILL?
  if (killAfterTicks_ > 0) {
    --killAfterTicks_;
    if (killAfterTicks_ == 0) {
      signal = SIGKILL;
    }
  }
  cpp2::KillRequest kill_req;
  while (queue_.read(kill_req)) {
    switch (kill_req.method) {
      case cpp2::KillMethod::KILL:
        signal = SIGKILL;  // KILL takes precedence over an outstanding TERM
        killAfterTicks_ = 0;  // No point in sending already-scheduled kills
        break;
      case cpp2::KillMethod::TERM_WAIT_KILL:
        if (signal == 0) {  // TERM does not replace an outstanding KILL
          signal = SIGTERM;
        }
        // Pick the earlier kill time of the two -- only one SIGKILL will fire.
        {
          uint32_t ticks = std::max(kill_req.killWaitMs, 0) / pollMs(opts_);
          killAfterTicks_ =
            (killAfterTicks_ == 0) ? ticks : std::min(ticks, killAfterTicks_);
        }
        break;
      case cpp2::KillMethod::TERM:
        if (signal == 0) {  // TERM does not replace an outstanding KILL
          signal = SIGTERM;
        }
        break;
      default:  // Not reached, checked in TaskSubprocessState::kill()
        LOG(FATAL) << "Unknown kill method: "
          << static_cast<int>(kill_req.method);
    }
  }
  // AsyncSubprocess guarantees that the process had not yet been wait()ed
  // for when this callback runs, so sendSignal() is not racy, and is
  // guaranteed to go to the right process / pgid.
  if (signal != 0) {
    // Do not signal the cgroup here because:
    //  - Well-behaved tasks will respond just fine to signaling the PGID.
    //  - Signaling only the cgroup takes longer and can fail in more ways.
    //  - Signaling both can confuse applications that have SIGTERM handling.
    // Therefore, cgroup killing only kicks in after the child has exited.
    CHECK(proc.returnCode().running());
    auto pid = proc.pid();
    CHECK(pid > 1);
    if (opts_.processGroupLeader) {
      pid = -pid;
      CHECK(pid < -1);
    }
    // FATAL since none of the POSIX error conditions can occur, unless we
    // have a serious bug like signaling the wrong PID.
    PLOG_IF(FATAL, ::kill(pid, signal) == -1)
      << "Failed to kill " << pid << " with " << signal;
    wasKilled_ = true;  // Alters "no status" handling, see above.
  } else if (--numPolls_ <= 0) {
    numPolls_ = getNumPolls();  // Reset the number of iterations to wait.
    try {
      resourceCallback_(rt, physicalResourceFetcher_.fetch());
    } catch (const std::exception& ex) {
      LOG(WARNING) << "Failed to fetch resources for task "
        << apache::thrift::debugString(rt) << ": " << ex.what();
    }
  }
}

void TaskSubprocessState::kill(cpp2::KillRequest req) {
  switch (req.method) {
    case cpp2::KillMethod::TERM_WAIT_KILL:
    case cpp2::KillMethod::KILL:
    case cpp2::KillMethod::TERM:
      if (!queue_.write(std::move(req))) {
        throw std::runtime_error("Failed to signal task, its queue is full.");
      }
      break;
    default:
      throw BistroException(
        "Unknown kill method: ", static_cast<int>(req.method)
      );
  }
}

} // namespace detail

}}  // namespace facebook::bistro
