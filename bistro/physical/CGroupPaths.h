/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <boost/filesystem/path.hpp>
#include <folly/Conv.h>
#include <folly/Optional.h>
#include <vector>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

struct CGroupPaths {
   // The full path to the a task cgroup is:
   //   /sys/fs/cgroup/<SUBSYSTEM>/path/to/bistro.slice/<TASK_SUBGROUP>
   // root: /sys/fs/cgroup
   // slice: path/to/bistro.slice
   // task_cgroup: shard:start:rand:worker_pid, **MUST NOT** contain slashes
  CGroupPaths(
    cpp2::CGroupOptions cgopts,
    // Optional since we want 'usable resources' in the absence of tasks.
    folly::Optional<boost::filesystem::path> task,
    // Let unit tests override this
    boost::filesystem::path numa_path = "/sys/devices/system/node/"
  ) : root_(std::move(cgopts.root)),
      slice_(std::move(cgopts.slice)),
      subsystems_(std::move(cgopts.subsystems)),
      task_(std::move(task)),
      numaPath_(std::move(numa_path)) {
    if (task_.has_value() && ++task_->begin() != task_->end()) {
      throw std::runtime_error(folly::to<std::string>(
        "'task' component of cgroup path must be a single, non-nested ",
        "directory, not: ", task_->native()
      ));
    }
  }

  boost::filesystem::path taskDir(const std::string& subsys) const {
    return root_ / subsys / slice_ / *task_;
  }

  boost::filesystem::path rootDir(const std::string& subsys) const {
    return root_ / subsys;
  }

  boost::filesystem::path sliceDir(const std::string& subsys) const {
    return root_ / subsys / slice_;
  }

  bool haveSubsystem(const std::string& subsystem) const {
    return subsystems_.end()
      != std::find(subsystems_.begin(), subsystems_.end(), subsystem);
  }

  boost::filesystem::path root_;
  boost::filesystem::path slice_;
  std::vector<std::string> subsystems_;
  folly::Optional<boost::filesystem::path> task_;  // A single directory
  boost::filesystem::path numaPath_;
};

// Avoid typos in subsystem & file names. Anon NS to honor ODR.
namespace {
  const char* const kSubSystemCpuSet = "cpuset";
  const char* const kSubSystemCpuAcct = "cpuacct";
  const char* const kSubSystemMemory = "memory";

  const char* const kCpuSetFile = "cpuset.cpus";
  const char* const kCpuMemFile = "cpuset.mems";
  const char* const kCpuUsageFile = "cpuacct.usage";
  const char* const kMemoryLimitFile = "memory.limit_in_bytes";
  const char* const kMemoryUseHierarchyFile = "memory.use_hierarchy";
  const char* const kMemoryUsageFile = "memory.usage_in_bytes";
  const char* const kProcsFile = "cgroup.procs";
}  // anonymous namespace

}}  // namespace facebook::bistro
