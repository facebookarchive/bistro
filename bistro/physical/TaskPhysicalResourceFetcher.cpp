/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/physical/TaskPhysicalResourceFetcher.h"

#include <chrono>

#include "bistro/bistro/physical/CGroupResources.h"
#include "bistro/bistro/physical/AllTasksPhysicalResourceMonitor.h"

namespace facebook { namespace bistro {

// Currently lacks a unit test, since this is some very simple glue code.

cpp2::TaskPhysicalResources TaskPhysicalResourceFetcher::fetch() const {
  cpp2::TaskPhysicalResources res;
  res.msSinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()
  ).count();
  if (auto maybe_ms = cgroups::taskCpuTimeMs(cgpaths_)) {
    res.cpuTimeMs_ref() = *maybe_ms;
  }
  if (auto maybe_mem_mb = cgroups::taskMemoryMB(cgpaths_)) {
    res.memoryMB_ref() = *maybe_mem_mb;
  }
  auto pids = cgroups::taskPids(cgpaths_);
  // Get GPUs, if GPU monitoring is enabled.
  if (auto all_task_resources = allTasksMonitor_->getDataOrThrow()) {
    res.gpus = all_task_resources->taskGpus(pids.begin(), pids.end());
  }
  return res;
}

}}  // namespace facebook::bistro
