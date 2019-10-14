/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/physical/UsablePhysicalResourceFetcher.h"

#include <folly/Conv.h>

#include "bistro/bistro/physical/utils.h"
#include "bistro/bistro/physical/CGroupResources.h"

namespace facebook { namespace bistro {

double systemRamMB() {
  const auto num_pages = sysconf(_SC_PHYS_PAGES);
  folly::checkUnixError(num_pages, "Cannot detect system memory pages");
  const auto page_size = sysconf(_SC_PAGESIZE);
  folly::checkUnixError(num_pages, "Cannot detect system memory page size");
  return (num_pages / 1024.) * (page_size / 1024.);
}

double UsablePhysicalResourceFetcher::memoryMB() const {
  // Base RAM amount: If `cpuset` is in use, it is authoritative. Otherwise,
  // just assume we have access to all of system memory.
  double mem_mb = [this]() {
    if (auto maybe_mem_mb = cgroups::usableNumaMemoryMB(cgpaths_)) {
      return *maybe_mem_mb;
    }
    return systemRamMB();
  }();
  if (auto maybe_mem_limit = cgroups::usableMemoryLimitMB(cgpaths_)) {
    mem_mb = std::min(mem_mb, *maybe_mem_limit);
  }
  return mem_mb;
}

uint32_t systemCPUCores() {
  auto num_cores = sysconf(_SC_NPROCESSORS_ONLN);
  folly::checkUnixError(num_cores, "Cannot detect system CPU cores");
  return num_cores;
}

uint32_t UsablePhysicalResourceFetcher::cpuCores() const {
  // If `cpuset` is in use, it is authoritative. Otherwise, query the system.
  if (auto maybe_cores = cgroups::usableCpuCores(cgpaths_)) {
    return *maybe_cores;
  }
  return systemCPUCores();
}

std::vector<cpp2::GPUInfo> UsablePhysicalResourceFetcher::gpus(
    uint32_t timeout_ms) const {
  std::vector<std::string> query_fields{"gpu_bus_id", "memory.total", "name"};
  std::vector<cpp2::GPUInfo> gpus;
  // Expected output:
  //   0000:28:00.0, 11519, Tesla K40m
  //   0000:0E:00.0, 12287, Quadro M6000
  for (const auto& l : queryNvidiaSmi("query-gpu", query_fields, timeout_ms)) {
    auto parts = checkedSplitLine(", ", l, query_fields.size());
    gpus.emplace_back();
    gpus.back().pciBusID = parts[0].str();
    gpus.back().name = parts[2].str();
    gpus.back().memoryMB = folly::to<double>(parts[1]);
    gpus.back().compute = 1.0;
  }
  return gpus;
}

}}  // namespace facebook::bistro
