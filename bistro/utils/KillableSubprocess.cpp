/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/KillableSubprocess.h"

#include <folly/ScopeGuard.h>
#include <folly/Subprocess.h>
#include <thread>

namespace facebook { namespace bistro {

namespace {
  // When waiting for a process to exit, poll this often.
  const int kPollPeriodMs = 20;
}

bool KillableSubprocess::Child::poll(bool* invoked_callback) {
  *invoked_callback = false;
  if (!subprocess_) {
    return true;  // Process already quit, don't call the termination callback.
  }
  auto debug_info = debugInfo();
  CHECK(subprocess_->returnCode().running()) << debug_info << "wasn't running";
  SCOPE_EXIT {
    if (!subprocess_->returnCode().running()) {
      subprocess_.reset();
    }
  };

  try {
    if (subprocess_->poll().running()) {
      return false;
    }
  } catch (const std::exception& e) {
    logException("Failed while polling task", e);  // Quite unlikely...
    throw;
  }
  terminationCB_(subprocess_, ExitReason::EXITED, debug_info);
  *invoked_callback = true;
  return true;
}

bool KillableSubprocess::Child::softKill(int ms_to_wait, int sig) {
  if (!subprocess_) {
    return false;  // Process already quit, don't call the termination callback
  }
  auto debug_info = debugInfo();
  CHECK(subprocess_->returnCode().running()) << debug_info << "wasn't running";
  SCOPE_EXIT { subprocess_.reset(); };

  // Soft-kill: send the ignorable signal (usually SIGTERM) and wait for the
  // process to exit.
  LOG(INFO) << "Soft-killing task " << debug_info << " with signal " << sig;
  try {
    subprocess_->sendSignal(sig);
  } catch (const std::exception& e) {
    LOG(WARNING) << "Failed to send signal " << sig << " to "  << debug_info
      << ", it may have just exited: " << e.what();
  }
  auto ret_code = subprocess_->poll();  // May throw
  int ms_waited = 0;
  while (ret_code.running() && ms_waited < ms_to_wait) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollPeriodMs));
    ms_waited += kPollPeriodMs;
    ret_code = subprocess_->poll();
  }

  // Hard-kill
  if (ret_code.running()) {
    LOG(WARNING) << "Child " << debug_info << " did not exit " << ms_to_wait
        << "ms after receiving " << sig << ", killing.";
    try {
      subprocess_->kill();
    } catch (const std::exception& e) {
      LOG(WARNING) << "Failed to SIGKILL " << debug_info << ", it may "
        << "have just exited: " << e.what();
    }
    subprocess_->wait();  // May throw
  }

  terminationCB_(subprocess_, ExitReason::SOFT_KILLED, debug_info);
  return true;
}

bool KillableSubprocess::Child::hardKill() {
  if (!subprocess_) {
    return false;  // Process already quit, don't call the termination callback
  }
  auto debug_info = debugInfo();
  CHECK(subprocess_->returnCode().running()) << debug_info << "wasn't running";
  SCOPE_EXIT { subprocess_.reset(); };

  LOG(INFO) << "Hard-killing task " << debug_info;
  try {
    subprocess_->kill();
  } catch (const std::exception& e) {
    LOG(WARNING) << "Failed to SIGKILL " << debug_info << ", it may "
      << "have just exited: " << e.what();
  }
  subprocess_->wait();  // May throw

  terminationCB_(subprocess_, ExitReason::HARD_KILLED, debug_info);
  return true;
}

bool KillableSubprocess::Child::sendSignal(int signum) noexcept {
  if (!subprocess_) {
    return false;  // Process already quit, don't signal it
  }
  auto debug_info = debugInfo();
  CHECK(subprocess_->returnCode().running()) << debug_info << "wasn't running";

  LOG(INFO) << "Sending signal " << signum << " to task " << debug_info;
  try {
    subprocess_->sendSignal(signum);
  } catch (const std::exception& e) {
    LOG(WARNING) << "Failed to send signal " << signum << " to "
      << debug_info << ", it may have just exited: " << e.what();
    return false;
  }
  return true;
}

std::string KillableSubprocess::Child::debugInfo() noexcept {
  return subprocess_
    ? folly::to<std::string>(debugID_, " with PID ", subprocess_->pid())
    : folly::to<std::string>(debugID_, " (not running)");
}

void KillableSubprocess::Child::logException(
    const std::string& msg,
    const std::exception& e) noexcept {

  auto debug_info = debugInfo();
  LOG(ERROR) << debug_info << ": " << msg << ": " << e.what();
  try {
    exceptionCB_(debug_info, msg, e);
  } catch (const std::exception& e2) {
    // "throw;" tends to follow logException(); it would be bad form to throw.
    LOG(ERROR) << "Yo dawg, yo' exception callback threw an exception while "
      << "recording an exception '" << e.what() << "' for " << debug_info
      << " after: " << msg << ": " << e2.what();
  }
}

void KillableSubprocess::communicate() {
  auto subprocess = child_->copySubprocessPtr();
  if (!subprocess) {
    return;  // Process was killed.
  }
  try {
    communicateCB_(subprocess);
  } catch (const std::exception& e) {
    child_->logException("Failed to communicate with task", e);
    throw;
  }
}

bool KillableSubprocess::pollAndSleep() {
  bool invoked_termination_callback;
  while (!child_->poll(&invoked_termination_callback)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollPeriodMs));
  }
  return invoked_termination_callback;
}

}}
