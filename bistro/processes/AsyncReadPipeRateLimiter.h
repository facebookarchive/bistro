/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncTimeout.h>

#include "bistro/bistro/processes/AsyncReadPipe.h"

namespace facebook { namespace bistro {

class AsyncReadPipeRateLimiter : public folly::AsyncTimeout {
public:
  // Negative quota is allowed -- for example, when used with
  // readPipeLinesCallback(), its internal buffering means that some lines
  // may arrive even after the pipes get paused.
  using Quota = int64_t;

  explicit AsyncReadPipeRateLimiter(
    folly::EventBase* event_base,
    uint32_t poll_every_ms,
    Quota quota_per_poll_interval,
    std::vector<std::shared_ptr<AsyncReadPipe>> pipes
  ) : AsyncTimeout(event_base),
      pollEveryMs_(poll_every_ms),
      quotaPerPollInterval_(quota_per_poll_interval),
      // Start with a nonzero quota, so that we don't block initially.
      quota_(quotaPerPollInterval_),
      pipes_(std::move(pipes)) {
    scheduleTimeout(pollEveryMs_);
  }

  // Subtract some amount from the current quota. If the quota becomes zero
  // or negative, the pipes get paused -- you may keep receiving data for a
  // while longer if your pipe callback is buffered.  Once enough time
  // passes that the quota becomes positive, the pipes are resumed.  Passing
  // a negative amount is not yet supported, and will not unpause the pipes.
  void reduceQuotaBy(Quota);

protected:
 void timeoutExpired() noexcept override; // Grow quota periodically.

private:
  const uint32_t pollEveryMs_;
  const Quota quotaPerPollInterval_;
  Quota quota_;  // Shared by all the pipes
  std::vector<std::shared_ptr<AsyncReadPipe>> pipes_;
};

}}  // namespace facebook::bistro
