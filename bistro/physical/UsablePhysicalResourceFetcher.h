/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/physical/CGroupPaths.h"

namespace facebook { namespace bistro {

// Fetches resources every time, no caching.
class UsablePhysicalResourceFetcher {
public:
  explicit UsablePhysicalResourceFetcher(CGroupPaths cgp)
    : cgpaths_(std::move(cgp)) {}

  double memoryMB() const;
  uint32_t cpuCores() const;
  std::vector<cpp2::GPUInfo> gpus(uint32_t timeout_ms) const;

private:
  const CGroupPaths cgpaths_;
};

}}  // namespace facebook::bistro
