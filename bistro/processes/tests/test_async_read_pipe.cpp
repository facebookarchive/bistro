/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include <unistd.h>

#include <folly/Exception.h>
#include <folly/File.h>
#include <folly/io/async/EventBase.h>
#include <folly/Random.h>

#include "bistro/bistro/processes/AsyncReadPipe.h"

using namespace facebook::bistro;

/**
 * Reads lines from a pipe via asyncReadPipe() and readFileLinesCallback().
 *
 * Writes message to a pipe in randomly sized chunks. Randomly alternates
 * between reading and writing (so that "read read write", and "write write
 * read" patterns are fairly probable, as are strict alternations).
 *
 * Checks that the lines conform to: a line always contains exactly one
 * delimiter (at the end of the line), except for EOF, in which case the
 * line has no delimiter.
 *
 * Checks that the concatenation of the lines reconstructs the message.
 */
void checkPipeRead(const std::string& message) {
  int pipe_fds[2];
  folly::checkUnixError(pipe(pipe_fds), "pipe");
  folly::File write_pipe(pipe_fds[1], /*owns_fd=*/ true);
  folly::File read_pipe(pipe_fds[0], /*owns_fd=*/ true);

  size_t cursor = 0;
  auto write_chunk_fn = [&]() {
    auto bytes_left = message.size() - cursor;
    if (!bytes_left) {
      write_pipe.close();  // idempotent
      return;
    }
    auto to_write = folly::Random::rand32(1, bytes_left + 1);
    auto written =
      folly::writeNoInt(write_pipe.fd(), message.data() + cursor, to_write);
    folly::checkUnixError(written, "write");
    LOG(INFO) << "wrote: '" << folly::StringPiece(
      message.data() + cursor, message.data() + cursor + written
    ) << "'";
    cursor += written;
  };

  folly::EventBase evb;
  int read_fd = read_pipe.fd();
  bool saw_eof = false;  // Ensure the callback sees EOF exactly once
  std::string read_message;  // Try to reconstruct the message
  auto closed = asyncReadPipe(
    &evb,
    std::move(read_pipe),
    readFileLinesCallback([&](folly::StringPiece s) {
      EXPECT_FALSE(saw_eof);  // Saw EOF at most once
      read_message.append(s.begin(), s.end());
      if (!s.empty()) {
        LOG(INFO) << read_fd << " read: '" << s << "'";
      }
      if (s.empty() || s.back() != '\n') {
        EXPECT_EQ(std::string::npos, folly::qfind(s, '\n'));  // No EOL at all
        LOG(INFO) << read_fd << " - EOF";
        saw_eof = true;
      } else {
        EXPECT_EQ(s.size() - 1, folly::qfind(s, '\n'));  // EOL only at the end
      }
      return true;  // Keep reading
    })
  );

  while (!closed.isReady()) {
    if (folly::Random::oneIn(2)) {
      write_chunk_fn();
    }
    if (folly::Random::oneIn(2)) {
      evb.loopOnce(EVLOOP_NONBLOCK);
    }
  }
  EXPECT_TRUE(saw_eof);
  EXPECT_EQ(message, read_message);
}

TEST(TestAsyncReadPipe, SimpleRead) {
  // Repeat the test a few times since it is randomized.
  for (size_t i = 0; i < 50; ++i) {
    checkPipeRead("");
    checkPipeRead("bunnies eat carrots and cabbage");
    checkPipeRead("\n\nbunnies\n\n\neat\ncarrots\nand\ncabbage\n\n");
  }
}

namespace { class TestError : public std::exception {}; }

TEST(TestAsyncReadPipe, CallbackException) {
  // Make a pipe and immediately close the write end, we won't need it.
  int pipe_fds[2];
  folly::checkUnixError(pipe(pipe_fds), "pipe");
  folly::File read_pipe(pipe_fds[0], /*owns_fd=*/ true);
  folly::checkUnixError(close(pipe_fds[1]));

  folly::EventBase evb;
  auto closed = asyncReadPipe(
    &evb,
    std::move(read_pipe),
    [&](const folly::File& f) -> bool { throw TestError(); }
  );
  while (!closed.isReady()) { evb.loop(); }
  EXPECT_THROW(closed.get(), TestError);
}
