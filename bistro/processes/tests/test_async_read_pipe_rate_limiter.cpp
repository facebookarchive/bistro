/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/experimental/AutoTimer.h>
#include <folly/io/async/EventBase.h>

#include "bistro/bistro/processes/AsyncReadPipeRateLimiter.h"
#include "bistro/bistro/processes/tests/utils.h"

using namespace facebook::bistro;

struct Pipe {
  Pipe(
    const char* name,
    folly::EventBase* evb,
    AsyncReadPipeRateLimiter::Quota quota_incr,
    folly::StringPiece to_write
  ) : name_(name), toWrite_(to_write) {
    folly::File read_pipe;
    makePipe(&read_pipe, &writePipe_);

    // Do not block on pipe writes
    int fd = writePipe_.fd();
    int flags = ::fcntl(fd, F_GETFL);
    folly::checkUnixError(flags, "fcntl");
    int r = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    folly::checkUnixError(r, "fcntl");

    pipes_.emplace_back(asyncReadPipe(
        evb,
        std::move(read_pipe),
        readPipeLinesCallback(
            [this](AsyncReadPipe* /*pipe*/, folly::StringPiece s) {
              if (!s.empty()) { // Don't count the "end of stream" empty string
                ++numRead_;
              }
              rateLim_->reduceQuotaBy(1);
            },
            0,
            '\n',
            10) // Buffer size of 10 lines
        ));
    rateLim_.reset(new AsyncReadPipeRateLimiter(evb, 1, quota_incr, pipes_));
  }

  void write() {
    auto ret =
      folly::writeNoInt(writePipe_.fd(), toWrite_.data(), toWrite_.size());
    if (ret != -1 || errno != EAGAIN) {
      folly::checkUnixError(ret, "write");
      toWrite_.advance(ret);
      LOG(INFO) << "Wrote " << ret << " lines to " << name_;
    }
  }

  const char* name_;
  folly::StringPiece toWrite_;
  folly::File writePipe_;
  std::unique_ptr<AsyncReadPipeRateLimiter> rateLim_;  // Initialized later
  std::vector<std::shared_ptr<AsyncReadPipe>> pipes_;
  size_t numRead_{0};
};

TEST(TestAsyncReadPipeRateLimiter, RateLimits) {
  folly::EventBase evb;
  std::string lines(200000, '\n');  // Usually exceeds pipe buffer size
  Pipe slow("slow", &evb, 1, lines);  // 1 line per 1 ms
  Pipe medium("medium", &evb, 20, lines);  // 20 lines per 1 ms => 10 sec
  Pipe fast("fast", &evb, 0, lines);  // no rate limit

  folly::AutoTimer<> timer;
  double runtime = 0;
  while (runtime < 1.0) {  // In 1 second, neither medium nor slow can finish
    slow.write();
    medium.write();
    fast.write();
    evb.loopOnce();
    runtime += timer.logFormat(
      "Read {} fast, {} medium, {} slow.",
      fast.numRead_, medium.numRead_, slow.numRead_
    ).count();
  }

  // Strictly faster than medium, 20KB/sec should not be too flaky in practice
  EXPECT_GT(fast.numRead_, 20000);  // Usually more like 200k
  EXPECT_LE(fast.numRead_, 200000);

  // Strictly faster than slow
  EXPECT_GT(medium.numRead_, 2000);  // Usually just under 20k
  EXPECT_LE(medium.numRead_, 20000);

  EXPECT_GT(slow.numRead_, 100);  // Usually just under 1000
  EXPECT_LE(slow.numRead_, 1000);
}
