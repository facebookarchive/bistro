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

#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/gen/String.h>
#include <folly/io/async/EventHandler.h>

namespace facebook { namespace bistro {

namespace detail { template <typename Callback> class AsyncReadPipeImpl; }
/**
 * You don't make one of these directly, instead use asyncReadPipe().  Once
 * the pipe handler is constructed, you can use these public APIs.
 */
class AsyncReadPipe : public folly::EventHandler {
public:
  // Insofar as I can tell, folly::Promise intends for this to be
  // thread-safe, meaning that you can call asyncReadPipe()->pipeClosed()
  // from outside the EventBase thread.
  folly::Future<folly::Unit> pipeClosed() { return closedPromise_.getFuture(); }

  ///
  /// None of the following are thread-safe, so you may only use them from
  /// this handler's EventBase thread -- most often, from the callback.
  ///

  bool isPaused() const { return isPaused_; }

  // The callback can use pipe().close() to stop reading early.
  folly::File& pipe() { return pipe_; }

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
      if (ret == -1 && errno == EAGAIN) {  // No more data for now
        break;
      }
      folly::checkUnixError(ret, "read");
      if (ret == 0) {  // Reached end-of-file
        splitter_.flush();  // Split whatever remained in the buffer
        pipe->pipe().close();
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
 * p->pipe().close(), which might cause the other end to get a SIGPIPE.
 *
 * The reason this exposes a shared_ptr is that we cannot resume() from
 * inside the callback -- it won't be called, and whatever code path is
 * responsible for resuming had better share ownership of this handler.
 * Note that you should generally only resume() from the EventBase thread.
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
    // Enable nonblocking IO
    int fd = pipe_.fd();
    int flags = ::fcntl(fd, F_GETFL);
    folly::checkUnixError(flags, "fcntl");
    int r = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    folly::checkUnixError(r, "fcntl");
    // Initialize the parent class
    initHandler(evb, fd);
    resume();
    // CAREFUL: we're not in the EventBase thread here, but handlerReady may
    // already be running, and this object may already be destroyed (!!!)
  }

  // Implementation detail -- invoke the "read" callback.
  // Final: it's unsafe to extend AsyncReadPipe, see CAREFUL above.
  void handlerReady(uint16_t events) noexcept override final {
    // These must be the FIRST lines in the scope, so that they run last.
    bool handler_should_die = false;
    auto suicide_guard = folly::makeGuard([&]() {
      if (handler_should_die) {
        unregisterHandler();
        // No longer self-owning. Will be destroyed here, or later, by
        // another owning thread.
        self_.reset();
      }
    });
    CHECK(events & EventHandler::READ);  // No WRITE support
    try {
      callback_(this);
      if (!pipe_) {  // The callback closed the pipe.
        handler_should_die = true;
        closedPromise_.setValue();
      }
    } catch (const std::exception& ex) {
      handler_should_die = true;
      folly::exception_wrapper ew{std::current_exception(), ex};
      try {
        pipe_.close();
      } catch (const std::exception& ex2) { // Not much to do but log
        LOG(ERROR) << "Caught '" << ex2.what() << "' while trying to close "
          << " pipe with FD " << pipe_.fd() << ") while handling callback "
          << "exception: " << ew.what();
      }
      closedPromise_.setException(ew);
    }
  }

private:
  // This object is self-owned, keeping it alive until we are sure none of
  // its code will run on the EventBase any more.
  std::shared_ptr<detail::AsyncReadPipeImpl<Callback>> self_;
  Callback callback_;
};
}  // namespace detail

}}  // namespace facebook::bistro
