/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <unistd.h>

#include <folly/Exception.h>
#include <folly/File.h>
#include <folly/io/async/EventBase.h>
#include <folly/Random.h>

#include "bistro/bistro/processes/AsyncReadPipe.h"
#include "bistro/bistro/processes/tests/utils.h"

using namespace facebook::bistro;

/**
 * Reads lines from a pipe via asyncReadPipe() and readPipeLinesCallback().
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
  folly::File read_pipe, write_pipe;
  makePipe(&read_pipe, &write_pipe);

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
    readPipeLinesCallback([&](AsyncReadPipe*, folly::StringPiece s) {
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
    })
  )->pipeClosed();

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
  folly::File read_pipe;
  makePipe(&read_pipe, nullptr);

  folly::EventBase evb;
  auto closed = asyncReadPipe(
    &evb,
    std::move(read_pipe),
    [&](AsyncReadPipe*) -> bool { throw TestError(); }
  )->pipeClosed();
  while (!closed.isReady()) { evb.loop(); }
  EXPECT_THROW(std::move(closed).get(), TestError);
}

TEST(TestAsyncReadPipe, PauseResume) {
  folly::File read_pipe, write_pipe;
  makePipe(&read_pipe, &write_pipe);

  size_t lines_before_pause = 3;
  folly::EventBase evb;
  auto pipe = asyncReadPipe(
    &evb,
    std::move(read_pipe),
    readPipeLinesCallback([&](AsyncReadPipe* pipe, folly::StringPiece s) {
      if (!s.empty()) {
        LOG(INFO) << "Read: '" << s << "'";
        --lines_before_pause;
        if (lines_before_pause == 0) {
          pipe->pause();
        }
      }
    }, 0, '\n', 1)  // 1-char buffer to ensure that pause is "instant"
  );
  auto closed = pipe->pipeClosed();

  auto write_fn = [&](const char* msg) {
    // Will deadlock if the pipe buffer fills up, but I'll take my chances.
    folly::writeFull(write_pipe.fd(), msg, strlen(msg));
  };

  evb.loopOnce(EVLOOP_NONBLOCK);  // No data, no action
  EXPECT_EQ(3, lines_before_pause);

  // The first 3 are consumed, and then we pause.
  for (int i = 0; i < 6; ++i) {
    write_fn("a\n");
    evb.loopOnce(EVLOOP_NONBLOCK);
    EXPECT_EQ((i < 3) ? (2 - i) : 0, lines_before_pause);
  }

  // Consume all the lines, and check that no more events fire.
  lines_before_pause = 5;
  pipe->resume();
  EXPECT_EQ(5, lines_before_pause);  // Doesn't fire without the EventBase
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(2, lines_before_pause);  // All 3 blocked lines get consumed
  evb.loopOnce(EVLOOP_NONBLOCK);  // Nothing more to read yet.
  write_fn("b\nc\nd\nf\ng\n");
  EXPECT_EQ(2, lines_before_pause);
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(0, lines_before_pause);  // 2 consumed, 3 to go

  lines_before_pause = 1;
  pipe->resume();
  EXPECT_EQ(1, lines_before_pause);
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(0, lines_before_pause);  // 1 consumed, 2 to go

  lines_before_pause = 2;
  pipe->resume();
  EXPECT_EQ(2, lines_before_pause);
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(0, lines_before_pause);  // All consumed

  EXPECT_FALSE(closed.isReady());
  write_pipe.close();
  EXPECT_FALSE(closed.isReady());  // The EventBase didn't run yet
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_FALSE(closed.isReady());  // The pipe is paused, so we didn't know
  pipe->resume();
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_TRUE(closed.isReady());  // Now we know
  EXPECT_EQ(0, lines_before_pause);  // No more lines were seen
}

TEST(TestAsyncReadPipe, ExternalClose) {
  folly::File read_pipe, write_pipe;
  makePipe(&read_pipe, &write_pipe);

  folly::EventBase evb;
  size_t num_lines = 0;
  auto pipe = asyncReadPipe(
      &evb,
      std::move(read_pipe),
      readPipeLinesCallback(
          [&](AsyncReadPipe* /*pipe*/, folly::StringPiece s) {
            if (!s.empty()) {
              LOG(INFO) << "Read: '" << s << "'";
              ++num_lines;
            }
          },
          0,
          '\n',
          1) // 1-char buffer to ensure that close is "instant"
      );
  auto closed = pipe->pipeClosed();

  evb.loopOnce(EVLOOP_NONBLOCK);  // No data, no action
  EXPECT_EQ(0, num_lines);
  EXPECT_FALSE(closed.isReady());

  auto write_fn = [&](const char* msg) {
    // Will deadlock if the pipe buffer fills up, but I'll take my chances.
    folly::writeFull(write_pipe.fd(), msg, strlen(msg));
  };

  // Feed through some lines without closing the pipe
  const int kMaxLines = 6;
  for (int i = 0; i < kMaxLines; ++i) {
    write_fn("a\n");
    evb.loopOnce(EVLOOP_NONBLOCK);
    EXPECT_EQ(i + 1, num_lines);
    EXPECT_FALSE(closed.isReady());
  }
  // These writes will get lost since we're synchronously closing the pipe.
  write_fn("b\n");
  write_fn("c\n");
  write_fn("d\n");
  pipe->close();
  EXPECT_TRUE(closed.isReady());

  // Nothing more happens on this event base, and we're the pipe's sole owner.
  evb.loopOnce(EVLOOP_NONBLOCK);
  EXPECT_EQ(kMaxLines, num_lines);
  EXPECT_EQ(1, pipe.use_count());
}
