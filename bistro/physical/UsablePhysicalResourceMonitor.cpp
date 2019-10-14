/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/physical/UsablePhysicalResourceMonitor.h"

#include <chrono>

#include "bistro/bistro/physical/UsablePhysicalResourceFetcher.h"

// Currently lacks a unit test, since the code is fairly simple, and it's
// quite annoying to test the update logic.

namespace facebook { namespace bistro {

namespace {
struct Fetcher {
  Fetcher(
    std::shared_ptr<folly::Synchronized<CGroupPaths>> cgpaths,
    uint32_t subprocess_timeout_ms
  ) : cgpaths_(std::move(cgpaths)),
      subprocessTimeoutMs_(subprocess_timeout_ms) {}

  bool operator()(
      std::shared_ptr<const cpp2::UsablePhysicalResources>* out,
      int*,  // unused fetch state; unused prev pointer
      std::shared_ptr<const cpp2::UsablePhysicalResources>) const {
    // Copy the latest CGroupPaths before fetching resources.
    folly::Optional<CGroupPaths> latest_cgpaths;
    SYNCHRONIZED(locked_cgpaths, *cgpaths_) {
      latest_cgpaths = locked_cgpaths;
    }
    UsablePhysicalResourceFetcher fetcher(std::move(*latest_cgpaths));
    auto res = std::make_shared<cpp2::UsablePhysicalResources>();
    res->msSinceEpoch =
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
    // In principle, cgroups can change our CPU or RAM allocation, so we
    // want to refresh these periodically.
    res->cpuCores = fetcher.cpuCores();
    res->memoryMB = fetcher.memoryMB();
    // nVidia GPUs can become 'lost' on the PCI bus, so also refresh.
    res->gpus = fetcher.gpus(subprocessTimeoutMs_);
    *out = res;
    return true;  // Got new data
  }
  std::shared_ptr<folly::Synchronized<CGroupPaths>> cgpaths_;
  const uint32_t subprocessTimeoutMs_;
};
}  // anonymous namespace

UsablePhysicalResourceMonitor::UsablePhysicalResourceMonitor(
  CGroupPaths cgpaths,
  uint32_t subprocess_timeout_ms,
  std::chrono::milliseconds period  // same delay on error as on success
) : cgpaths_(
      std::make_shared<folly::Synchronized<CGroupPaths>>(std::move(cgpaths))
    ),
    poller_(
      "UsablePhysRsrcs",
      Fetcher(cgpaths_, subprocess_timeout_ms),
      period,
      period
    ) {
}

}}  // namespace facebook::bistro
