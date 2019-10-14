/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/processes/AsyncReadPipeRateLimiter.h"

namespace facebook { namespace bistro {

void AsyncReadPipeRateLimiter::reduceQuotaBy(Quota how_much) {
  Quota next = quota_ - how_much;
  if (next <= 0 && quota_ > 0) {  // Quota just ran out
    for (auto&& pipe : pipes_) {
      pipe->pause();
    }
  }
  if (next < quota_) {  // Avoid underflow, ignore negative decrements
    quota_ = next;
  }
}

void AsyncReadPipeRateLimiter::timeoutExpired() noexcept {
  // Increase the quota and resume all pipes if it becomes positive,
  Quota next = quota_ + quotaPerPollInterval_;
  if (quota_ <= 0 && next > 0) {
    for (auto&& pipe : pipes_) {
      pipe->resume();  // reduceQuotaBy() pauses the pipes.
    }
  }
  // This check prevents quota overflow, and, if the increment is not
  // positive, effectively disables the quota.
  quota_ = (next > quota_) ? next : std::numeric_limits<Quota>::max();
  scheduleTimeout(pollEveryMs_);
}

}}  // namespace facebook::bistro
