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
#include <iostream>
#include <string>
#include <vector>

#include "bistro/bistro/utils/TemporaryFile.h"
#include "bistro/bistro/utils/KillableSubprocess.h"
#include <folly/Conv.h>
#include <folly/Optional.h>
#include <folly/Subprocess.h>
#include <folly/Synchronized.h>

DECLARE_int32(child_sigterm_wait_ms);

namespace facebook { namespace bistro {

enum class LogTable : unsigned char {
  STDERR = 0,
  STDOUT = 1,
  STATUSES = 2,
};

/**
 * Wraps running a process with some extras:
 * - Allows you to pass a filename to the process for status communcation.
 * - Uses a callback to save lines from the process's stdout & stderr, and
 *   to log its status changes.
 *
 * Like folly::Subprocess, you must reap the process before destroying the
 * object.  Any of wait(), softKill(), or hardKill() is sufficient.
 *
 * Warning: not thread-safe, except for softKill() and hardKill().
 */
class ProcessRunner {

private:
  struct Child;

  typedef std::function<void(LogTable, folly::StringPiece)> WriteCallback;

public:
  /**
   * Starts the process in such a way that it is safe to *Kill() it from
   * another thread.  You must call wait() or *Kill() before destruction.
   *
   * Throws on failure (e.g. executable not found).
   */
  ProcessRunner(
    WriteCallback write_cb,
    const boost::filesystem::path& child_working_dir,
    const std::vector<std::string>& cmd,
    std::pair<
      std::vector<std::string>,
      std::unique_ptr<TemporaryFile>
    >&& args_and_pipe
  );

  /**
   * Blocks until the process exits, in such a way that it's safe to kill()
   * it from another thread.
   *
   * If the wait() reaps the child, returns the status string that the
   * process wrote.  If the child is *Kill()ed, returns none.
   *
   * Throws on failure to read the status, or errors in the callback.
   */
  folly::Optional<std::string> wait() {
    return killable_.wait() ? exitStatus_ : folly::none;;
  }

  /**
   * If the child is running, soft-kills it, and returns the status string
   * the process wrote.  Returns none if the process was not running.
   * Throws on errors, e.g. failing to read the status, or errors in the
   * callback.
   *
   * Thread-safety: Can be called from a different thread than the one
   * calling wait().
   */
  folly::Optional<std::string> softKill() {
    return killable_.softKill(FLAGS_child_sigterm_wait_ms)
      ? exitStatus_ : folly::none;;
  }

  /**
   * Just like softKill, but sends SIGKILL immediately.
   */
  folly::Optional<std::string> hardKill() {
    return killable_.hardKill() ? exitStatus_ : folly::none;;
  }

private:

  // Makes the working directory, starts the subprocess.
  std::unique_ptr<folly::Subprocess> makeSubprocess(
    WriteCallback write_cb,
    const boost::filesystem::path& child_working_dir,
    const std::vector<std::string>& cmd,
    const std::vector<std::string>& args
  );

  // Clone of KillableSubprocess::Child::logException to use when the
  // killable_ is not readily available.
  void logException(
    const folly::StringPiece debug_info,
    const folly::StringPiece msg,
    const std::exception& e
  ) noexcept;

  // Used both by KillableSubprocess and by logException; may throw.
  void exceptionCallback(
    const folly::StringPiece debug_info,
    const folly::StringPiece msg,
    const std::exception& e
  );

  // TODO: This is clowntown -- this should really just be a path to
  // /proc/self/fd/<N> to the write side of a pipe created by
  // Subprocess. When doing that, take note that TemporaryFile::readline()
  // currently limits status lines to 4kb.  The pipe reader has to do
  // something similar to avoid filling up the status DB.
  std::unique_ptr<TemporaryFile> pipeFile_;

  WriteCallback writeCB_;
  folly::Optional<std::string> exitStatus_;

  // Must be initialized last, since initialization starts the subprocess.
  KillableSubprocess killable_;
};

}}
