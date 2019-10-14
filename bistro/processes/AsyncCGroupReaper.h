/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/futures/Promise.h>
#include <folly/io/async/AsyncTimeout.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

/**
 * A task subprocess can exit, while leaving behind other descendants that
 * are still running.  Normal POSIX process handling has no way of tracking
 * or reaping these, but Linux CGroups enable it.
 *
 * asyncCGroupReaper() makes a self-owning, EventBase-dwelling callback that:
 *  - Returns a future, which is fulfilled only when **all** of the task's
 *    cgroups become empty (or are deleted by the system `release_agent`).
 *  - Tries to `rmdir` any cgroups that become empty, in case the system
 *    `release_agent` is not enabled.
 *  - Logs about the intransigent task to raise awareness of leaky tasks.
 *  - If a `freezer` cgroup is available or if `killWithoutFreezer` is set,
 *    SIGKILLs processes in that cgroup.
 *
 * All of these functions are executed on an exponential backoff schedule,
 * since we expect that tasks stuck in D state (e.g.  due to buggy kernel
 * drivers) can persist for a long time.
 */
folly::Future<folly::Unit> asyncCGroupReaper(
  folly::EventBase* event_base,
  cpp2::CGroupOptions cgopts,
  std::string cgname,
  uint32_t min_wait_ms,  // Set to pollMs(TaskSubprocessOptions)
  // Only D-state tasks should end up retrying for a long time.  Note that
  // this should not be overly long, since freezing + signaling takes at
  // least two calls to workOnKillingCGroupTasks().
  uint32_t max_wait_ms = 10000
);

}}  // namespace facebook::bistro
