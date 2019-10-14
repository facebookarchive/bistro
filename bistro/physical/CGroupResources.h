/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "bistro/bistro/physical/CGroupPaths.h"

namespace facebook { namespace bistro { namespace cgroups {

folly::Optional<double> usableNumaMemoryMB(const CGroupPaths& cgpaths);
folly::Optional<double> usableMemoryLimitMB(const CGroupPaths& cgpaths);
folly::Optional<uint32_t> usableCpuCores(const CGroupPaths& cgpaths);

folly::Optional<double> taskMemoryMB(const CGroupPaths& cgpaths);
folly::Optional<uint64_t> taskCpuTimeMs(const CGroupPaths& cgpaths);
std::unordered_set<pid_t> taskPids(const CGroupPaths& cgpaths);

}}}  // namespace facebook::bistro::cgroups
