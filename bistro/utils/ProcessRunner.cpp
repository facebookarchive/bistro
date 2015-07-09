/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/ProcessRunner.h"

#include <boost/filesystem.hpp>

#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>

#include "bistro/bistro/utils/hostname.h"
#include "bistro/bistro/utils/Exception.h"

DEFINE_int32(
  child_sigterm_wait_ms, 10000,
  "How long to wait for a child to exit after SIGTERM before sending SIGKILL"
);

DEFINE_int32(
  task_parent_death_signal, 9,   // README.task_termination explains the KILL
  "(Linux-specific) If Bistro exits, what signal should its child processes "
  "receive? Use 0 for 'no signal', 15 for SIGTERM, 9 for SIGKILL."
);

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {

string debugID(const vector<string>& cmd, const vector<string>& args) {
  return folly::to<string>(
    "['", folly::join("', '", cmd), "'] + [", folly::join("', '", args), "]"
  );
}

}  // anonymous namespace

ProcessRunner::ProcessRunner(
    WriteCallback write_cb,
    const boost::filesystem::path& child_working_dir,
    const vector<string>& cmd,
    // WARNING: args_and_pipe.second gets moved, so don't use it.
    pair<vector<string>, unique_ptr<TemporaryFile>>&& args_and_pipe)
  : pipeFile_(std::move(args_and_pipe.second)),
    writeCB_(write_cb),
    killable_(
      makeSubprocess(write_cb, child_working_dir, cmd, args_and_pipe.first),
      // Communicate callback
      [this](shared_ptr<Subprocess> subprocess) {
        auto read_cb = Subprocess::readLinesCallback(
          [this](int fd, StringPiece s) {
            if (s.empty()) {
              // Skip the empty line at the end of \n-terminated files
              return false;
            }
            if (fd == STDERR_FILENO) {
              writeCB_(LogTable::STDERR, s);
            } else if (fd == STDOUT_FILENO) {
              writeCB_(LogTable::STDOUT, s);
            }  // Else: we don't care about (and shouldn't have) other FDs
            return false;
          },
          65000  // Limit line length to protect the log DB
        );
        subprocess->communicate(
          std::ref(read_cb),  // See comment on Subprocess::ReadLinesCallback
          [](int pdf, int cfd){ return true; }  // Don't write to the child
        );
      },
      // Exception callback
      std::bind(
        &ProcessRunner::exceptionCallback,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3
      ),
      // Termination callback
      [this](
        shared_ptr<Subprocess>& subprocess,
        KillableSubprocess::ExitReason exit_reason,
        const StringPiece debug_info
      ) {
        // Before recording the status, log the reason for the task's exit.
        writeCB_(
          LogTable::STATUSES,
          // This is deliberately not a JSON blob nor any other kind of
          // TaskStatus, so that if somebody attempts to parse the log, they
          // don't mistake it for a valid status.
          KillableSubprocess::getExitReasonStr(exit_reason)
        );

        // Capture the process's status string even if it was killed.  That
        // way, soft-kill's SIGTERM gives a task an opportunity to
        // checkpoint, or even to call itself done.
        string status;  // Write an empty status if a pipe file isn't in use
        try {
          if (pipeFile_) {
            status = pipeFile_->readline();  // Reads up to 4kb
          }
        } catch (const exception& e) {
          logException(debug_info, "Failed to read process status", e);
          throw;
        }
        writeCB_(LogTable::STATUSES, status);
        exitStatus_ = status;
      },
      debugID(cmd, args_and_pipe.first)
    ) {
}

unique_ptr<Subprocess> ProcessRunner::makeSubprocess(
    WriteCallback write_cb,
    const boost::filesystem::path& working_dir,
    const vector<string>& cmd,
    const vector<string>& args) {

  CHECK(cmd.size() >= 1);
  vector<string> full_cmd{cmd};
  full_cmd.insert(full_cmd.end(), args.begin(), args.end());

  writeCB_(LogTable::STATUSES, folly::toJson(dynamic::object
    ("result", "running")
    // The worker host need not be the same as the scheduler host, so it's
    // important to log it.
    ("data", dynamic::object("worker_host", getLocalHostName()))
  ).toStdString());
  try {
    try {
      boost::filesystem::create_directories(working_dir);
    } catch (const boost::filesystem::filesystem_error& e) {
      throw BistroException("Failed to make working directory: ", e.what());
    }
    auto opts =
      Subprocess::pipeStdout().pipeStderr().chdir(working_dir.native());
    if (FLAGS_task_parent_death_signal != 0) {
      opts.parentDeathSignal(FLAGS_task_parent_death_signal);
    }
    return make_unique<Subprocess>(full_cmd, opts);
  } catch (const exception& e) {
    logException(debugID(cmd, args), "Failed to start task", e);
    throw;
  }
}

void ProcessRunner::logException(
    const StringPiece debug_info,
    const StringPiece msg,
    const exception& e) noexcept {

  LOG(ERROR) << debug_info << ": " << msg << ": " << e.what();
  try {
    exceptionCallback(debug_info, msg, e);
  } catch (const exception& e2) {
    // "throw;" tends to follow logException(); it would be bad form to throw.
    LOG(ERROR) << "Yo dawg, yo' exception callback threw an exception while "
      << "recording an exception '" << e.what() << "' for " << debug_info
      << " after: " << msg << ": " << e2.what();
  }
}

void ProcessRunner::exceptionCallback(
    const StringPiece debug_info,
    const StringPiece msg,
    const exception& e) {

  // It's useful for the task log to show errors like "File not found",
  // "Couldn't create directory", etc.
  writeCB_(LogTable::STATUSES, folly::toJson(dynamic::object
    ("result", "backoff")
    ("data", dynamic::object("error", folly::to<string>(msg, ": ", e.what())))
  ).toStdString());
}

}}
