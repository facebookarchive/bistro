/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Synchronized.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/physical/CGroupPaths.h"
#include "bistro/bistro/utils/PeriodicPoller.h"

namespace facebook { namespace bistro {

class UsablePhysicalResourceMonitor {
public:
  UsablePhysicalResourceMonitor(
    CGroupPaths,
    uint32_t subprocess_timeout_ms,
    std::chrono::milliseconds period
  );

  std::shared_ptr<const cpp2::UsablePhysicalResources> getDataOrThrow() const {
    return poller_.getDataOrThrow();
  }

  // This lets us live-update the cgroup paths, enabling Bistro workers'
  // usage of cgroups to be reconfigured (with care!!!) while they run.
  void updateCGroupPaths(CGroupPaths cgpaths) {
    SYNCHRONIZED(locked_cgpaths, *cgpaths_) {
      locked_cgpaths = std::move(cgpaths);
    }
  }

private:
  static std::shared_ptr<const cpp2::UsablePhysicalResources> processRawData(
    const std::shared_ptr<const cpp2::UsablePhysicalResources>,  // unused prev
    const std::shared_ptr<const cpp2::UsablePhysicalResources>& cur
  ) { return cur; }  // No-op

  const std::shared_ptr<folly::Synchronized<CGroupPaths>> cgpaths_;

  PeriodicPoller<
    // Each refresh makes a new const struct, to be used as long as needed.
    std::shared_ptr<const cpp2::UsablePhysicalResources>,
    int,  // Unused 'state' -- we have no fetch/process distinction.
    cpp2::UsablePhysicalResources,
    UsablePhysicalResourceMonitor::processRawData  // No-op
  > poller_;
};

}}  // namespace facebook::bistro
