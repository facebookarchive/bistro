/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
    res.cpuTimeMs = *maybe_ms;
    res.__isset.cpuTimeMs = true;
  }
  if (auto maybe_mem_mb = cgroups::taskMemoryMB(cgpaths_)) {
    res.memoryMB = *maybe_mem_mb;
    res.__isset.memoryMB = true;
  }
  auto pids = cgroups::taskPids(cgpaths_);
  // Get GPUs, if GPU monitoring is enabled.
  if (auto all_task_resources = allTasksMonitor_->getDataOrThrow()) {
    res.gpus = all_task_resources->taskGpus(pids.begin(), pids.end());
  }
  return res;
}

}}  // namespace facebook::bistro
