/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
