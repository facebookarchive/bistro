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

/**
 * Construct via readFileLinesCallback() to get template deduction.
 *
 * A callback that helps you read lines (or other character-delimited
 * pieces) from a pipe/file.  Your callback gets chunks with the delimiter
 * still attached, unless this is the final piece in the stream.  If the
 * last character is a delimiter, your callback will get an empty last
 * piece, so an EOF signal is guaranteed.
 *
 * A schematic example (see the unit test's checkPipeRead() for a full one):
 *
 *   asyncReadPipe(
 *     event_base_ptr,
 *     folly::File(pipe_fd, true),  // owns_fd
 *     readFileLinesCallback([fd](folly::StringPiece line) {
 *       std::cout << "read line from fd " << fd << ": " << line;
 *       return true;  // Keep reading
 *     })
 *   ).get();
 *
 * If an input line exceeds maxLineLength, your callback will get some
 * initial chunks of maxLineLength with no trailing delimiters.  The final
 * chunk of a line is delimiter-terminated iff the delimiter was present in
 * the input.  In particular, the last line always lacks a delimiter -- so
 * if the stream ends on a delimiter, the final line is empty.
 */
template <typename LineCallback> class ReadFileLinesCallback;
template <typename LineCallback>
ReadFileLinesCallback<LineCallback> readFileLinesCallback(
    LineCallback line_cob,
    uint64_t max_line_length = 0,  // No line length limit by default
    char delimiter = '\n',
    uint64_t buf_size = 1024) {
  return ReadFileLinesCallback<LineCallback>(
    std::move(line_cob), max_line_length, delimiter, buf_size
  );
}
template <typename LineCallback>
class ReadFileLinesCallback {
public:
  explicit ReadFileLinesCallback(
    LineCallback line_cob,
    uint64_t max_line_length = 0,  // No line length limit by default
    char delimiter = '\n',
    uint64_t buf_size = 1024
  ) : bufSize_(buf_size),
      splitter_(delimiter, std::move(line_cob), max_line_length) {}

  bool operator()(const folly::File& file) {
    char buf[bufSize_];
    while (true) {  // Read until EAGAIN or EOF or callback stop request
      ssize_t ret = folly::readNoInt(file.fd(), buf, bufSize_);
      if (ret == -1 && errno == EAGAIN) {  // No more data for now
        return true;
      }
      folly::checkUnixError(ret, "read");
      if (ret == 0) {  // Reached end-of-file
        splitter_.flush();  // Ignore return since the file is over anyway
        return false;
      }
      if (!splitter_(folly::StringPiece(buf, ret))) {
        return false;  // The callback told us to stop
      }
    }
  }

private:
  const uint64_t bufSize_;
  folly::gen::StreamSplitter<LineCallback> splitter_;
};

/**
 * Read from a pipe via an EventBase (regular files are not supported).
 * asyncReadPipe() need not be run from the EventBase thread.
 *
 * The resulting future is ready when the pipe is closed.  It yields an
 * exception if your callback throws (in which case the pipe is also closed,
 * ignoring and logging secondary exceptions), or if closing the pipe
 * throws.
 *
 * Your read callback runs in the thread of the specified EventBase:
 *
 *   bool (const folly::File&)
 *
 * Return true for "keep reading" or false for "close FD and stop reading",
 * which may cause the other end to get a SIGPIPE.
 */
namespace detail { template <typename Callback> class AsyncReadPipeHandler; }
template <typename Callback>
folly::Future<folly::Unit> asyncReadPipe(
    folly::EventBase* evb, folly::File pipe, Callback cob) {
  folly::Future<folly::Unit> f;
  // I'm not leaking the object; it's self-owned, and self-destructs on
  // close.  It would be *unsafe* to inspect the returned pointer, since it
  // might already have been deleted.  This arrangement makes sense for a
  // few reasons:
  //  - The handler must have a stable address to be used with EventBase.
  //  - External ownership makes it possible for the object to be destroyed
  //    right before libevent invokes its callback (use-after-free).
  new detail::AsyncReadPipeHandler<Callback>(
    evb, std::move(pipe), std::move(cob), &f
  );
  return f;
}
namespace detail {
template <typename Callback>
class AsyncReadPipeHandler : public folly::EventHandler {
protected:
  friend folly::Future<folly::Unit> asyncReadPipe<>(
    folly::EventBase*, folly::File, Callback
  );

  AsyncReadPipeHandler(
    folly::EventBase* evb,
    folly::File pipe,
    Callback cob,
    folly::Future<folly::Unit>* pipe_closed
  ) : pipe_(std::move(pipe)), callback_(std::move(cob)) {
    *pipe_closed = closedPromise_.getFuture();
    // Enable nonblocking IO
    int fd = pipe_.fd();
    int flags = ::fcntl(fd, F_GETFL);
    folly::checkUnixError(flags, "fcntl");
    int r = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    folly::checkUnixError(r, "fcntl");
    // Initialize the parent class
    initHandler(evb, fd);
    // This CHECK will abort on regular files.
    CHECK(registerHandler(EventHandler::READ | EventHandler::PERSIST));
    // CAREFUL: we're not in the EventBase thread here, but handlerReady may
    // already be running, and this object may already be destroyed (!!!)
  }

  // Implementation detail -- invoke the "read" callback.
  // Final: it's unsafe to extend AsyncReadPipeHandler, see CAREFUL above.
  void handlerReady(uint16_t events) noexcept override final {
    // These must be the FIRST lines in the scope, so that they run last.
    bool handler_should_die = false;
    auto suicide_guard = folly::makeGuard([&]() {
      if (handler_should_die) {
        unregisterHandler();
        delete this;
      }
    });
    CHECK(events & EventHandler::READ);  // No WRITE support
    try {
      // Ensure that the callback cannot close the pipe.
      const folly::File& const_pipe = pipe_;
      if (callback_(const_pipe)) {  // Keep reading?
        return;
      }
      // The callback requested the pipe to be closed.
      handler_should_die = true;
      closedPromise_.setWith([this]() { pipe_.close(); });
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
  folly::File pipe_;
  Callback callback_;
  folly::Promise<folly::Unit> closedPromise_;
};
}  // namespace detail

}}  // namespace facebook::bistro
