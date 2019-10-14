/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/physical/CGroupPaths.h"
#include "bistro/bistro/utils/PeriodicPoller.h"

namespace facebook { namespace bistro {

/*
 * Some resources, like nVidia GPUs, cannot be efficiently be queried
 * per-task, and are best queried from this global cache.
 *
 * We deliberately do *not* record per-PID memory utilization, since we
 * always use GPUs in an exclusive setting, and the per-PID RAM usage is
 * lower than the total RAM usage on the GPU.  So we report the higher
 * number to be on the safe side.
 */
struct AllTasksPhysicalResources {
  // Given iterators into a container of task PIDs, fetch all GPUs (and
  // their stats) that are being used by the task.
  template <typename PidIterT>
  std::vector<cpp2::GPUInfo> taskGpus(PidIterT first, PidIterT last) const {
    // Multiple PIDs of one task *could* be reported for a GPU.
    std::unordered_set<const cpp2::GPUInfo*> gpu_ptrs;
    gpu_ptrs.reserve(gpuInfos_.size());  // Avoid reallocs
    for (; first != last; ++first) {
      auto it = pidToGpuInfos_.find(*first);
      if (it != pidToGpuInfos_.end()) {
        for (const auto* gpu_ptr : it->second) {
          gpu_ptrs.insert(gpu_ptr);
        }
      }
    }
    std::vector<cpp2::GPUInfo> gpus;
    gpus.reserve(gpu_ptrs.size());
    for (const auto* gpu_ptr : gpu_ptrs) {
      gpus.emplace_back(*gpu_ptr);
    }
    return gpus;
  }

  std::vector<cpp2::GPUInfo> gpuInfos_;  // Fully populated GPUInfo structs
  std::unordered_map<pid_t, std::vector<const cpp2::GPUInfo*>>
    pidToGpuInfos_;  // pointers into gpuInfos_ (a pid can use many GPUs)
};

class AllTasksPhysicalResourceMonitor {
public:
  AllTasksPhysicalResourceMonitor(
    uint32_t subprocess_timeout_ms, std::chrono::milliseconds period
  );

  std::shared_ptr<const AllTasksPhysicalResources> getDataOrThrow() const {
    return poller_.getDataOrThrow();
  }

private:
  static std::shared_ptr<const AllTasksPhysicalResources> processRawData(
    const std::shared_ptr<const AllTasksPhysicalResources>,  // unused prev ptr
    const std::shared_ptr<const AllTasksPhysicalResources>& cur
  ) { return cur; }  // No-op

  PeriodicPoller<
    // Each refresh makes a new const struct, to be used as long as needed.
    std::shared_ptr<const AllTasksPhysicalResources>,
    int,  // Unused 'state' -- we have no fetch/process distinction.
    AllTasksPhysicalResources,
    AllTasksPhysicalResourceMonitor::processRawData  // No-op
  > poller_;
};

}}  // namespace facebook::bistro
