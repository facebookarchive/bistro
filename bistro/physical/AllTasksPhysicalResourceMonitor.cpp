/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/physical/AllTasksPhysicalResourceMonitor.h"

#include "bistro/bistro/physical/utils.h"

namespace facebook { namespace bistro {

namespace {
std::vector<cpp2::GPUInfo> fetchGpuUtilization(uint32_t timeout_ms) {
  std::vector<cpp2::GPUInfo> gpus;
  std::vector<std::string> query_fields{
    "gpu_bus_id", "memory.used", "utilization.gpu", "name"
  };
  // Expected output:
  //   0000:28:00.0, 298, 28, Tesla K40m
  //   0000:30:00.0, 4591, 95, Quadro M6000
  for (const auto& l : queryNvidiaSmi("query-gpu", query_fields, timeout_ms)) {
    auto parts = checkedSplitLine(", ", l, query_fields.size());
    gpus.emplace_back();
    gpus.back().pciBusID = parts[0].str();
    gpus.back().memoryMB = folly::to<double>(parts[1]);
    gpus.back().compute = folly::to<double>(parts[2]) / 100;  // was in percent
    gpus.back().name = parts[3].str();
  }
  return gpus;
}

std::unordered_set<std::pair<pid_t, std::string>> fetchGpuPidsWithBusID(
    uint32_t timeout_ms) {
  std::unordered_set<std::pair<pid_t, std::string>> pid_gpus;
  std::vector<std::string> query_fields{"pid", "gpu_bus_id"};
  // Expected output:
  //   60982, 0000:28:00.0
  //   462446, 0000:30:00.0
  for (const auto& l
        : queryNvidiaSmi("query-compute-apps", query_fields, timeout_ms)) {
    auto parts = checkedSplitLine(", ", l, query_fields.size());
    if (!pid_gpus.emplace(
      std::make_pair(folly::to<pid_t>(parts[0]), parts[1].str())
    ).second) {
      throw BistroException("nvidia-smi printed a pid+bus ID pair twice: ", l);
    }
  }
  return pid_gpus;
}

struct Fetcher {
  explicit Fetcher(uint32_t timeout_ms) : subprocessTimeoutMs_(timeout_ms) {}
  bool operator()(
      std::shared_ptr<const AllTasksPhysicalResources>* out,
      int*,  // unused fetch state
      std::shared_ptr<const AllTasksPhysicalResources>) {  // unused prev ptr
    auto res = std::make_shared<AllTasksPhysicalResources>();
    // DANGER: Make sure this will not get realloc'ed below, since we are
    // about to take pointers into it.
    res->gpuInfos_ = fetchGpuUtilization(subprocessTimeoutMs_);
    // Make a map from bus ID to a pointer into gpuInfos_.
    std::unordered_map<std::string, const cpp2::GPUInfo*> bus_id_to_gpu_info;
    for (const auto& gpu_info : res->gpuInfos_) {
      bus_id_to_gpu_info[gpu_info.pciBusID] = &gpu_info;
    }
    // Fetch the pid => bus ID list, and translate it into gpuInfos_ pointers.
    for (const auto& p : fetchGpuPidsWithBusID(subprocessTimeoutMs_)) {
      auto it = bus_id_to_gpu_info.find(p.second);
      if (it == bus_id_to_gpu_info.end()) {
        throw BistroException("PID ", p.first, " uses bad bus ID ", p.second);
      }
      res->pidToGpuInfos_[p.first].emplace_back(it->second);
    }
    *out = res;
    return true;  // Got new data
  }
  const uint32_t subprocessTimeoutMs_;
};
}  // anonymous namespace

AllTasksPhysicalResourceMonitor::AllTasksPhysicalResourceMonitor(
  uint32_t subprocess_timeout_ms,
  std::chrono::milliseconds period  // same delay on error as on success
) : poller_(
  "AllTaskPhysRsrc", Fetcher(subprocess_timeout_ms), period, period
) {
}

}}  // namespace facebook::bistro
