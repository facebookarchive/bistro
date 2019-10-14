/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <folly/experimental/TestUtil.h>

namespace facebook { namespace bistro {

// Callback for CaptureFD to improve debuggability
void printString(folly::StringPiece s);

/**
 * Incrementally reads from fd until the entire text consumed thus far
 * matches the regex.  CAREFUL: You are not guaranteed to stop consuming
 * exactly when the regex matches, so it's easy to over-consume and cause
 * the next such wait to fail.
 */
void waitForRegexOnFd(folly::test::CaptureFD* fd, std::string regex);


// Timing utilities

using TestClock = std::chrono::high_resolution_clock;
using TestTimePoint = std::chrono::time_point<TestClock>;

inline double timeDiff(TestTimePoint a, TestTimePoint b) {
  return
    std::chrono::duration_cast<std::chrono::duration<double>>(a - b).count();
}
inline double timeSince(TestTimePoint t) {
  return timeDiff(TestClock::now(), t);
}

}}  // namespace facebook::bistro
