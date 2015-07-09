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

#include <folly/Range.h>
#include <folly/Synchronized.h>
#include <memory>
#include <csignal>
#include <string>

namespace folly {
  class Subprocess;
}

namespace facebook { namespace bistro {

/**
 * Single-use wrapper around folly::Subprocess, which makes it safe to
 * wait() from one thread but kill the process from another thread.  Also
 * supports soft-kill ([ignorable signal like SIGTERM] - sleep - SIGKILL).
 *
 * Note: wait() is not reentrant, only the *Kill() functions are safe.
 *
 * Like folly::Subprocess, the process starts the moment you construct the
 * object.  Similarly, you **must** reap the process before destruction.
 * Any of wait(), softKill(), or hardKill() will suffice.
 *
 * Design notes:
 *
 * Using folly::Subprocess's wait() and kill() is not safe because UNIX has
 * an irreconcilable race between blocking waitpid() and kill().  In a
 * multithreaded setting, the kill may run after the process has already
 * been reaped by the blocking wait.  It would then fail, or, since PIDs get
 * recycled, kill some unrelated process.
 *
 * This implementation relies on Subprocess's reasonable, but undocumented
 * behavior: it is currently safe to signal the child from one thread, while
 * another thread executes communicate() or poll().
 *
 * If useful, it would be completely reasonable to expose poll(), so that
 * the polling could be done in some event loop, e.g. via folly::EventBase.
 */
class KillableSubprocess {
public:
  enum class ExitReason { EXITED, SOFT_KILLED, HARD_KILLED };

  static std::string getExitReasonStr(ExitReason exit_reason) {
    switch(exit_reason) {
      case ExitReason::EXITED:
        return "exited";
      case ExitReason::SOFT_KILLED:
        return "soft-killed";
      case ExitReason::HARD_KILLED:
        return "hard-killed";
      default:
        LOG(FATAL) << "Unknown exit reason: " << static_cast<int>(exit_reason);
        return "this value is not used";
    }
  }

private:
  typedef std::function<void(
    std::shared_ptr<folly::Subprocess>  // copy since this is unsynchronized
  )> CommunicateCallback;

  typedef std::function<void(
    const folly::StringPiece debug_info,
    const folly::StringPiece msg,
    const std::exception& e
  )> ExceptionCallback;

  typedef std::function<void(
    std::shared_ptr<folly::Subprocess>&,  // ref ok, always synchronized
    ExitReason exit_reason,
    const folly::StringPiece debug_info
  )> TerminationCallback;

public:
  KillableSubprocess(
    std::unique_ptr<folly::Subprocess>&& subprocess,
    // This is where you invoke Subprocess::communicate() or similar.  You
    // must not call Subprocess::poll() or wait() or anything else that
    // might reap the child process, because this is called outside of the
    // mutex that ensures that pollAndSleep() does not race *Kill().
    //
    // CAUTION: May run concurrently with exception_cb or terminate_cb.
    CommunicateCallback communicate_cb,
    // KillableSubprocess makes an honest attempt to log all common
    // exceptions and causes of failures.  Whatever it logs, it also feeds
    // to this callback, so that you'd can to log it on a per-task basis.
    //
    // CAUTION: May run concurrently with communicate_cb or terminate_cb.
    ExceptionCallback exception_cb,
    // Called right after the suprocess exits or is killed -- your chance to
    // retrieve the child's exit status or do cleanup.
    //
    // CAUTION: May be called from either *Kill()'s or wait()'s thread.
    // May run concurrently with communicate_cb or exception_cb.
    TerminationCallback termination_cb,
    // Used in logging output to help you differentiate subprocesses.  The
    // "debug info" received by the callbacks includes this ID.
    const std::string& debug_id
  ) : child_(Child(
        std::move(subprocess), termination_cb, exception_cb, debug_id
      )),
      communicateCB_(communicate_cb) {}

  /**
   * Blocks until the process exits, in such a way that it's safe to *Kill()
   * it from another thread. Not reentrant.
   *
   * If the process exits due to a *Kill() call, then *Kill() invokes the
   * termination callback.  Otherwise, wait() will invoke the termination
   * callback.
   *
   * Returns true if this call invoked the termination callback.
   *
   * Strategy for *Kill thread-safety:
   *
   * 1) Without locking child_, blocks in Subprocess::communicate() until
   *    the subprocess closes its stdout / stderr pipes.
   * 2) Loops lock-poll-unlock-sleep until the process exits.  This ensures
   *    that we never signal the child after waitpid() reaps it, but *Kill()
   *    can still succeed in a timely fashion.
   */
  bool wait() {
    communicate();
    return pollAndSleep();
  }

  /**
   * If the process is not running, does nothing and returns false.
   * Otherwise, sends SIGTERM (optionally, another signal) to the child,
   * waits up to ms_to_wait for it to exit, and sends SIGKILL if it does
   * not.  After reaping the process, invokes the termination callback, and
   * returns true.
   *
   * NB It's possible that the exit reason we pass to the callback will be
   * "killed" even if the process naturally exited just before signaled it.
   * That's works fine, aside from the wrong reason being logged.
   */
  bool softKill(int ms_to_wait, int signum = SIGTERM) {
    return child_->softKill(ms_to_wait, signum);
  }

  /**
   * Just like softKill(), but sends SIGKILL right away.
   */
  bool hardKill() { return child_->hardKill(); }

  /**
   * Subprocess::sendSignal protected from the waitpid() race, and with
   * logging.  Returns true if the signal was sent.
   */
  bool sendSignal(int signum) noexcept { return child_->sendSignal(signum); }

private:
  /**
   * Provides mutual exclusion for *Kill() and poll(), and protection for
   * the relevant data members.
   *
   * Core invariant: subprocess_ is cleared the moment the process is reaped
   * either by *Kill() or poll().
   */
  struct Child {
    Child(
      std::unique_ptr<folly::Subprocess>&& subprocess,
      TerminationCallback termination_cb,
      ExceptionCallback exception_cb,
      const std::string& debug_id
    ) : subprocess_(std::move(subprocess)),
        terminationCB_(termination_cb),
        exceptionCB_(exception_cb),
        debugID_(debug_id) {}

    /**
     * Returns false if the process is still running.  Invokes the
     * termination callback if this poll reaps the process.  Sets the
     * boolean argument accordingly.
     */
    bool poll(bool* invoked_termination_callback);

    // See the docblock for KillableSubprocess::softKill()
    bool softKill(int ms_to_wait, int signum);

    // See the docblock for KillableSubprocess::hardKill()
    bool hardKill();

    // See the docblock for KillableSubprocess::sendSignal()
    bool sendSignal(int signum) noexcept;

    std::string debugInfo() noexcept;

    void logException(const std::string& msg, const std::exception& e)
      noexcept;

    std::shared_ptr<folly::Subprocess> copySubprocessPtr() {
      return subprocess_;
    }

    // This is a shared_ptr because we must not hold the mutex while blocked
    // in communicate() -- so the unsynchronized code uses a copy of this
    // pointer.  Set to null as soon as the process exits.
    std::shared_ptr<folly::Subprocess> subprocess_;

    const TerminationCallback terminationCB_;
    const ExceptionCallback exceptionCB_;
    const std::string debugID_;
  };

  /**
   * Blocking, but does not hold a lock on child_. This is fine, since
   * Subprocess::communicate() does not call waitpid(), so it cannot race
   * with *Kill().  This also assumes that it's safe to signal the
   * subprocess while another thread is executing communicate().
   */
  void communicate();

  /**
   * Alternates between calling Child::poll() and sleeping, until the
   * process exits.  Invokes the termination callback if this poll reaps the
   * process.
   */
  bool pollAndSleep();

  folly::Synchronized<Child> child_;
  // This callback is outside of Child since communicate() must be
  // unsynchronized, or we couldn't kill a communicating process.
  const CommunicateCallback communicateCB_;
};

}}
