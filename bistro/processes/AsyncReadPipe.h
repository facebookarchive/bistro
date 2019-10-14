/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/File.h>
#include <folly/Exception.h>
#include <folly/FileUtil.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/gen/String.h>
#include <folly/io/async/EventHandler.h>

namespace facebook { namespace bistro {

namespace detail { template <typename Callback> class AsyncReadPipeImpl; }
/**
 * You don't make one of these directly, instead use asyncReadPipe() below.
 * Once the pipe handler is constructed, you can use these public APIs.
 */
class AsyncReadPipe : public folly::EventHandler {
public:
  // Insofar as I can tell, folly::Promise intends for this to be
  // thread-safe, meaning that you can call asyncReadPipe()->pipeClosed()
  // from outside the EventBase thread.
  folly::Future<folly::Unit> pipeClosed() {
    return closedPromise_.getFuture();
  }

  ///
  /// None of the following are thread-safe, so you may only use them from
  /// this handler's EventBase thread -- most often, from the callback.
  ///

  bool isPaused() const { return isPaused_; }

  // Do NOT use this to close the FD or modify its flags.
  const folly::File& pipe() const { return pipe_; }

  // Immediately closes `pipe()` and lets `AsyncReadPipe` be destroyed as
  // soon as all of its external references go away.
  //
  // Can throw -- you are going to get the exception immediately, and not
  // via the pipeClosed() future.
  //
  // Safe to call from the read callback, or from another callback on the
  // same EventBase.
  void close() { closeImpl(); }

  void pause() {
    if (!isPaused_) {
      isPaused_ = true;
      unregisterHandler();
    }
  }

  // Caveat: Once the pipe is paused, the pipe won't invoke the callback, so
  // you cannot use it to resume reading.  Instead, use the shared_ptr
  // returned by asyncReadPipe() to resume() via *another* call on the
  // EventBase thread.  Since the EventHandler is paused, you can probably
  // even get away with calling resume() from another thread, but I would
  // avoid it, since it's easy to get the synchronization wrong.
  void resume() {
    if (isPaused_) {
      isPaused_ = false;
      // If we don't check `pipe_`, a well-intentioned resume would abort if
      // the pipe had already been closed (e.g. due to an exception in the
      // read callback).
      if (pipe_) {
        // This CHECK will abort on regular files.
        CHECK(registerHandler(EventHandler::READ | EventHandler::PERSIST));
      }
    }
  }

protected:
  template <typename Callback> friend class AsyncReadPipeImpl;
  explicit AsyncReadPipe(folly::File pipe) : pipe_(std::move(pipe)) {}

  virtual void closeImpl() = 0;

  folly::Promise<folly::Unit> closedPromise_;
  folly::File pipe_;
  bool isPaused_{true};
};

/**
 * Construct via readPipeLinesCallback() to get template deduction.
 *
 * A callback that helps you read lines (or other character-delimited
 * pieces) from an AsyncReadPipe.  Your callback gets chunks with the
 * delimiter still attached, unless this is the final piece in the stream.
 * If the last character is a delimiter, your callback will get an empty
 * last piece, so an EOF signal is guaranteed.
 *
 * A schematic example (see the unit test's checkPipeRead() for a full one):
 *
 *   asyncReadPipe(
 *     event_base_ptr,
 *     folly::File(pipe_fd, true),  // owns_fd
 *     readPipeLinesCallback([fd](AsyncReadPipe* p, folly::StringPiece line) {
 *       std::cout << "read line from fd " << p->pipe.fd() << ": " << line;
 *     })
 *   ).get();
 *
 * If an input line exceeds maxLineLength, your callback will get some
 * initial chunks of maxLineLength with no trailing delimiters.  The final
 * chunk of a line is delimiter-terminated iff the delimiter was present in
 * the input.  In particular, the last line always lacks a delimiter -- so
 * if the stream ends on a delimiter, the final line is empty.
 *
 * WARNING: If your PipeLineCallback calls pause(), it must still consume
 * any lines that are already in the StreamSplitter's internal buffer.  So,
 * do NOT rely on "pause()" working instantly here.
 */
template <typename PipeLineCallback> class ReadPipeLinesCallback;
template <typename PipeLineCallback>
ReadPipeLinesCallback<PipeLineCallback> readPipeLinesCallback(
    PipeLineCallback pipe_line_cob,
    uint64_t max_line_length = 0,  // No line length limit by default
    char delimiter = '\n',
    uint64_t buf_size = 1024) {
  return ReadPipeLinesCallback<PipeLineCallback>(
    std::move(pipe_line_cob), max_line_length, delimiter, buf_size
  );
}
template <typename PipeLineCallback>
class ReadPipeLinesCallback {
  // This `bind`-like wrapper lets us pass the AsyncReadPipe* through the
  // StreamSplitter to the PipeLineCallback.  Decidedly not thread-safe.
  struct BindPipePtr {
    BindPipePtr(PipeLineCallback cob, AsyncReadPipe** pipe_ptr)
      : cob_(std::move(cob)), pipePtr_(pipe_ptr) {}
    bool operator()(folly::StringPiece s) {
      cob_(*pipePtr_, s);
      // Continue splitting even if the pipe is paused, since a
      // StreamSplitter cannot be paused.
      return true;
    }
    PipeLineCallback cob_;
    AsyncReadPipe** pipePtr_;
  };

public:
  explicit ReadPipeLinesCallback(
    PipeLineCallback line_cob,
    uint64_t max_line_length = 0,  // No line length limit by default
    char delimiter = '\n',
    uint64_t buf_size = 1024
  ) : pipePtr_(new (AsyncReadPipe*)),
      bufSize_(buf_size),
      splitter_(
        delimiter,
        BindPipePtr(std::move(line_cob), pipePtr_.get()),
        max_line_length
      ) {}

  void operator()(AsyncReadPipe* pipe) {
    // This kludge passes `pipe` into the inner callback through the
    // StreamSplitter.  It seems better than threading templated varargs
    // through the entire StreamSplitter implementation.
    *pipePtr_ = pipe;
    char buf[bufSize_];
    // Read until EAGAIN or EOF, or the callback closes the FD or pauses the
    // pipe.  It's crucial to check isPaused here -- otherwise we might read
    // indefinitely even after the client requests a pause.
    while (pipe->pipe() && !pipe->isPaused()) {
      ssize_t ret = folly::readNoInt(pipe->pipe().fd(), buf, bufSize_);
      if (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;  // No more data for now
      }
      folly::checkUnixError(ret, "read");
      if (ret == 0) {  // Reached end-of-file
        splitter_.flush();  // Split whatever remained in the buffer
        pipe->close();
        break;
      }
      splitter_(folly::StringPiece(buf, ret));
    }
  }

private:
  std::unique_ptr<AsyncReadPipe*> pipePtr_;  // unique_ptr for a fixed address
  const uint64_t bufSize_;
  folly::gen::StreamSplitter<BindPipePtr> splitter_;
};

/**
 * Read from a pipe via an EventBase (regular files are not supported).
 * asyncReadPipe() need not be run from the EventBase thread.
 *
 * AsyncReadPipe::pipeClosed() becomes ready when the pipe is closed.  It
 * yields an exception if your callback throws (in which case the pipe is
 * also closed, ignoring and logging secondary exceptions).
 *
 * Your read callback runs in the thread of the specified EventBase:
 *
 *   void (const AsyncReadPipe* p)
 *
 * It may call p->pause() to temporarily cease being called, or even
 * p->close(), which might cause the other end to get a SIGPIPE.
 *
 * The reason this exposes a shared_ptr is that we cannot resume() from
 * inside the callback -- it won't be called, and whatever code path is
 * responsible for resuming had better share ownership of this handler.
 *
 * Not thread-safe: only resume() or close() from the EventBase thread.
 */
template <typename Callback>
std::shared_ptr<AsyncReadPipe> asyncReadPipe(
    folly::EventBase* evb, folly::File pipe, Callback cob) {
  std::shared_ptr<AsyncReadPipe> p;
  // I'm not leaking the object; it's self-owned, and self-destructs on
  // close.  It would be *unsafe* to use the returned pointer, since it
  // already belongs to a shared_ptr.  This arrangement makes sense for a
  // few reasons:
  //  - The handler must have a stable address to be used with EventBase.
  //  - Purely external ownership would allow the object to be destroyed
  //    right before libevent invokes its callback (use-after-free).
  (new detail::AsyncReadPipeImpl<Callback>(std::move(pipe), std::move(cob)))
    ->initialize(evb, &p);
  return p;
}

namespace detail {


template <typename Callback>
class AsyncReadPipeImpl : public AsyncReadPipe {
protected:
  friend std::shared_ptr<AsyncReadPipe> asyncReadPipe<>(
    folly::EventBase*, folly::File, Callback
  );

  AsyncReadPipeImpl(folly::File pipe, Callback cob)
    : AsyncReadPipe(std::move(pipe)), self_(this), callback_(std::move(cob)) {}

  // Avoid "exception in constructor" shenanigans by two-stage initialization.
  void initialize(
      folly::EventBase* evb,
      std::shared_ptr<AsyncReadPipe>* handler) {
    *handler = self_;
    // Mark the FD nonblocking. This affects all other copies of the FD, so
    // this can hurt other processes that do blocking I/O on it.  We assume
    // that since this is the read end of a pipe, we are the sole owner.
    int fd = pipe_.fd();
    int flags = ::fcntl(fd, F_GETFL);
    folly::checkUnixError(flags, "fcntl get flags");
    int r = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    folly::checkUnixError(r, "fcntl set flags");
    // Initialize the parent class
    initHandler(evb, folly::NetworkSocket::fromFd(fd));
    resume();
    // CAREFUL: we're not in the EventBase thread here, but handlerReady may
    // already be running, and this object may already be destroyed (!!!)
  }

  // Implementation detail -- invoke the "read" callback.
  // Final: it's unsafe to extend AsyncReadPipe, see CAREFUL above.
  void handlerReady(uint16_t events) noexcept final {
    // Must be FIRST line to trigger self-destruction as late as possible.
    SelfGuard self_guard(self_);
    CHECK(events & EventHandler::READ);  // No WRITE support
    try {
      callback_(this);
      if (!pipe_) {  // The callback closed the pipe.
        self_guard.dieSoon();
      }
    } catch (const std::exception& ex) {
      folly::exception_wrapper ew{std::current_exception(), ex};
      try {
        pipe_.close();
      } catch (const std::exception& ex2) { // Not much to do but log
        LOG(ERROR) << "Caught '" << ex2.what() << "' while trying to close "
          << " pipe with FD " << pipe_.fd() << ") while handling callback "
          << "exception: " << ew.what();
      }
      self_guard.dieSoon(std::move(ew));
    }
  }

  void closeImpl() override {
    if (self_) {  // Double-close is not an error
      // Must be FIRST line to trigger self-destruction as late as possible.
      SelfGuard self_guard(self_);
      self_guard.dieSoon();  // Request destruction first, close() can throw.
      pipe_.close();
    }
  }

private:
  using SharedPtr = std::shared_ptr<detail::AsyncReadPipeImpl<Callback>>;

  // Must be declared first in the function. This guard serves two purposes:
  //  1) Ensure that self-destruction (`self_.reset()`) is always the last
  //     thing that happens in `handlerReady()`, preventing use-after-free.
  //  2) Ensure that `closeImpl()` is safe to call from `handlerReady()`,
  //     allowing `AsyncReadPipe::close()` to work correctly from inside a
  //     read callback or from outside.  This is why this copies `self`.
  class SelfGuard {
  public:
    explicit SelfGuard(SharedPtr self) : self_(self) {
      CHECK(self_);
      ++self_->guardNestingDepth_;
    }
    ~SelfGuard() {
      CHECK_GT(self_->guardNestingDepth_, 0);
      --self_->guardNestingDepth_;
      if (deathException_.hasValue()) {
        // No longer self-owned. SelfGuard::self_ may hold the last reference.
        self_->self_.reset();
        // Unregister & fulfill the promise eagerly in case something
        // external to AsyncReadPipe is extending our lifetime.
        self_->unregisterHandler();
        // Promises can be fulfilled only once, so the outermost call does it.
        if (!self_->guardNestingDepth_) {
          if (*deathException_) {
            self_->closedPromise_.setException(*deathException_);
          } else {
            self_->closedPromise_.setValue();
          }
        } else {
          CHECK(!*deathException_) << "Only handlerReady reports exceptions";
        }
      } else {
        CHECK(self_->self_) << "dieSoon never triggers ONLY in a nested call";
      }
    }
    void dieSoon(folly::exception_wrapper ew = folly::exception_wrapper()) {
      deathException_ = ew;
    }
  private:
    // folly::none => don't die, no exception => die with no error.
    folly::Optional<folly::exception_wrapper> deathException_;
    SharedPtr self_;
  };

  friend class SelfGuard;

  // This object is self-owned, keeping itself alive until we can be sure
  // none of its code will run on the EventBase any more. Invariant:
  // self_ is non-null so long as pipe_ is still open.
  SharedPtr self_;
  Callback callback_;
  // With e.g. `closeImpl()` called from `handlerReady()`, we want the
  // outermost function to fulfill the promise.  This beats doing it in the
  // destructor, since external holders of the shared_ptr can delay our
  // actual destruction arbitrarily long, and deadlock on the future.
  size_t guardNestingDepth_{0};
};

}  // namespace detail

}}  // namespace facebook::bistro
