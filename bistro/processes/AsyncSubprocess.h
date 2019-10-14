/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/Subprocess.h>

namespace facebook { namespace bistro {

/**
 * A lightweight alternative to folly::MPMCQueue for SendSignalCallback.
 * Multiple signal requests **will** get merged into one if they occur in
 * within the span of a single AsyncSubprocess timeout.
 */
class OneSignalFlag {
public:
  explicit OneSignalFlag(int signum) : signal_(signum) {}
  // Not thread-safe, for initialization convenience only.
  OneSignalFlag(OneSignalFlag&& other) noexcept : signal_(other.signal_) {
    if (other.sendNoSignal_.test_and_set()) {
      sendNoSignal_.test_and_set();
    } else {
      sendNoSignal_.clear();
    }
  }
  void sendSignal() { sendNoSignal_.clear(); }  // Thread-safe
  bool read(int& signal) {
    if (sendNoSignal_.test_and_set()) {
      return false;
    }
    signal = signal_;
    return true;
  }
private:
  /* const but movable */ int signal_;
  std::atomic_flag sendNoSignal_{true};
};

/**
 * If all you need is to be able to send your asyncSubprocess() signals from
 * from any other thread, this is the RuntimeCallback for you.
 *
 * The SignalQueue may be a folly::MPMCQueue<int>, or see OneSignalFlag for
 * a super-lighweight quack-alike -- the test demonstrates both.
 *
 * Choosing a pointer type: if your queue can be owned externally, and will
 * outlive the AsyncSubprocess's EventBase, use a raw pointer.  If the
 * AsyncSubprocess might outlive the caller, use a shared_ptr.
 *
 * Use sendSignalCallback() for template deduction.
 */
template <typename SignalQueuePtr> class SendSignalCallback;
template <typename SignalQueuePtr>
SendSignalCallback<SignalQueuePtr> sendSignalCallback(SignalQueuePtr q) {
  return SendSignalCallback<SignalQueuePtr>(q);
}
namespace detail { template <typename RuntimeCallback> class AsyncSubprocess; }
template <typename SignalQueuePtr>
class SendSignalCallback {
public:
  explicit SendSignalCallback(SignalQueuePtr queue) : queue_(queue) {}
protected:
  friend class detail::AsyncSubprocess<SendSignalCallback<SignalQueuePtr>>;
  // Called serially from the AsyncSubprocess's EventBase thread.
  void operator()(folly::Subprocess& proc) noexcept {
    int signal;
    while (queue_->read(signal)) {
      // Theoretically, this may throw, but if it does, abort() is best.
      proc.sendSignal(signal);
    }
  }
private:
  SignalQueuePtr queue_;
};

/**
 * Waits (in a non-blocking fashion) for a Subprocess using an EventBase.
 * asyncSubprocess() need not be run from the EventBase thread.
 *
 * The resulting future is ready when the Subprocess::returnCode() is no
 * longer RUNNING -- i.e.  after we successfully waitpid() the child.  The
 * future never yields an exception.
 *
 * Provides a RuntimeCallback, which MUST BE noexcept, to let you run custom
 * logic (possibly controlled by other threads, with appropriate
 * synchronization) in this AsyncSubprocess's EventBase thread.  Search the
 * unit test for `SendSignalCallback` to see a couple of good ownership &
 * synchronization patterns.
 *
 * Your callback runs after every Subprocess::poll(), which returns RUNNING.
 * Since the callback is serialized with poll(), it can safely sendSignal().
 * If your process exits quickly, the callback might NEVER get executed.
 *
 * CAUTION: If you want a blocking operation in your callback, you're doing
 * it wrong (and blocking up the EventBase thread).  Instead, try again on
 * the callback's next invocation.
 *
 * Rationale:
 *  - Actions on the process must go through a callback because it is unsafe
 *    to signal a process from a thread other than the waiting one -- the
 *    signal may fire after the wait succeeds, and kill the wrong process.
 *  - The callback is a low-level, compile-time bound mixin because I don't
 *    want to dictate how you want to communicate / synchronize with other
 *    threads.  SendSignalCallback shows a simple example.
 */
template <typename RuntimeCallback>
folly::Future<folly::ProcessReturnCode> asyncSubprocess(
    folly::EventBase* evb,
    folly::Subprocess proc,
    RuntimeCallback runtime_cob,
    uint32_t poll_ms = 10) {
  // At the time of writing, Future isn't default-constructible.
  folly::Optional<folly::Future<folly::ProcessReturnCode>> f;
  // I'm not leaking the object; it's self-owned, and self-destructs on
  // close.  It would be *unsafe* to inspect the returned pointer, since it
  // might already have been deleted.  This arrangement makes sense for a
  // few reasons:
  //  - The handler must have a stable address to be used with EventBase.
  //  - External ownership makes it possible for the object to be destroyed
  //    right before libevent invokes its callback (use-after-free).
  (new detail::AsyncSubprocess<RuntimeCallback>(
    evb, std::move(proc), std::move(runtime_cob), poll_ms
  ))->initialize(&f);
  return std::move(f.value());
}
namespace detail {
template <typename RuntimeCallback>
class AsyncSubprocess : public folly::AsyncTimeout {
protected:
  friend folly::Future<folly::ProcessReturnCode> asyncSubprocess<>(
    folly::EventBase*, folly::Subprocess, RuntimeCallback, uint32_t
  );

  AsyncSubprocess(
    folly::EventBase* event_base,
    folly::Subprocess proc,
    RuntimeCallback runtime_cob,
    uint32_t poll_ms
  ) : AsyncTimeout(event_base),
      pollEveryMs_(poll_ms),
      runtimeCallback_(std::move(runtime_cob)),
      subprocess_(std::move(proc)) {}

  // Since Promise has a nontrivial destructor, it may be bad to `delete
  // this` in the constructor, possibly even "undefined behavior"-bad :)
  void initialize(
    folly::Optional<folly::Future<folly::ProcessReturnCode>>* return_code
  ) {
    *return_code = returnCode_.getFuture();
    scheduleTimeout(pollEveryMs_);
    // CAREFUL: we're not in the EventBase thread here, so timeoutExpired
    // may already be running, and this may already be destroyed (!!!)
  }

  void timeoutExpired() noexcept override {
    // No try-except since poll() only throws on usage errors.
    auto ret = subprocess_.poll();
    if (UNLIKELY(!ret.running())) {
      returnCode_.setValue(std::move(ret));
      delete this;  // If setValue() throws we don't leak since we abort() :)
      return;  // No more timeouts, the object was deleted.
    }
    runtimeCallback_(subprocess_);  // 'noexcept' by contract
    scheduleTimeout(pollEveryMs_);
  }

private:
  const uint32_t pollEveryMs_;
  RuntimeCallback runtimeCallback_;
  folly::Subprocess subprocess_;
  folly::Promise<folly::ProcessReturnCode> returnCode_;
};
}  // namespace detail

}}  // namespace facebook::bistro
