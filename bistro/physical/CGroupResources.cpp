/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/physical/CGroupResources.h"

#include <folly/File.h>
#include <folly/gen/File.h>

#include "bistro/bistro/physical/utils.h"

namespace facebook { namespace bistro { namespace cgroups {

namespace {
// "", "a", "a/b", ... The first partial is empty.  The last one is `p`.
std::vector<boost::filesystem::path> partialPaths(
    const boost::filesystem::path& p) {
  std::vector<boost::filesystem::path> partials{""};
  for (const auto& part : p) {
    partials.emplace_back(partials.back() / part);
  }
  return partials;
}
}  // anonymous namespace

// Scan cpuset subsystem (from slice up to the root) for cpuset.mems files.
// The first non-empty one found gives the memory limit.
folly::Optional<double> usableNumaMemoryMB(const CGroupPaths& cgpaths) {
  if (!cgpaths.haveSubsystem(kSubSystemCpuSet)) {
    return folly::none;
  }

  double mem_mb = 0;
  auto partials = partialPaths(cgpaths.slice_);
  auto base = cgpaths.rootDir(kSubSystemCpuSet);
  for (auto rit = partials.rbegin(); rit != partials.rend(); ++rit) {
    auto mem_nodes_str = valueFromFile<std::string>(base / *rit / kCpuMemFile);
    if (mem_nodes_str.empty()) {
      continue;  // Look higher up the tree.
    }
    // Query NUMA memory nodes: /sys/devices/system/node/node<num>/meminfo
    for (const auto node : parseIntSet(mem_nodes_str)) {
      // Format:
      //   Node 0 MemTotal:       134217724 kB
      //   Node 0 MemFree:         4307508 kB
      //   ...
      try {
        auto node_id = folly::to<std::string>(node);
        const auto mem_total_parts =
          folly::gen::byLine(folly::File(
            (cgpaths.numaPath_ /  ("node" + node_id) / "meminfo").native()))
          | folly::gen::filter([](folly::StringPiece p) {
              return p.contains("MemTotal:");  // Find the MemTotal row
            })
          | folly::gen::resplit(' ')  // Tokenize the MemTotal row
          | folly::gen::filter([](folly::StringPiece p) { return !p.empty(); })
          | folly::gen::eachTo<std::string>()
          | folly::gen::as<std::vector>();
        if (mem_total_parts.size() != 5
            || mem_total_parts[0] != "Node"
            || mem_total_parts[1] != node_id
            || mem_total_parts[2] != "MemTotal:"
            || mem_total_parts[4] != "kB") {
          throw BistroException(
            "Unknown meminfo format: ", folly::join(' ', mem_total_parts)
          );
        }
        mem_mb += folly::to<uint64_t>(mem_total_parts[3]) / 1024.;
      } catch (const std::exception& ex) {
        throw BistroException(
          "Failed to read meminfo for node ", node, ": ", ex.what()
        );
      }
    }
    return mem_mb;
  }
  throw BistroException("All cpuset.mems files were empty");
}

// Scan memory subsystem (from root down to slice) for memory.limit_in_bytes
// files.  On each level check another file memory.use_hierarchy.  If it
// exists and has value "1", return the minimum limit from that level until
// the slice.  If no cgroup on the path to the task sets
// memory.use_hierarchy, then no memory limit is set.
folly::Optional<double> usableMemoryLimitMB(const CGroupPaths& cgpaths) {
  if (!cgpaths.haveSubsystem(kSubSystemMemory)) {
    return folly::none;
  }

  uint64_t mem_bytes = ~0;  // No limit by default
  bool use_hierarchy = false;
  auto base = cgpaths.rootDir(kSubSystemMemory);
  for (const auto& slicePartial : partialPaths(cgpaths.slice_)) {
    // Check memory.use_hierarchy file, if we did not find it yet.
    if (!use_hierarchy) {
      use_hierarchy =
        valueFromFile<bool>(base / slicePartial / kMemoryUseHierarchyFile);
    }
    // Pick the smallest memory.limit_in_bytes below the use_hierarchy dir.
    if (use_hierarchy) {
      mem_bytes = std::min(
        mem_bytes,
        valueFromFile<uint64_t>(base / slicePartial / kMemoryLimitFile)
      );
    }
  }

  if (!use_hierarchy) {
    // Without hierarchy, none of the root through slice limits are
    // enforced, and only the task limit can have any effect.
    return folly::none;
  }
  CHECK(mem_bytes > 0);
  return mem_bytes / (1024. * 1024.);
}

// Get available CPU cores through the `cpuset` subsystem.
folly::Optional<uint32_t> usableCpuCores(const CGroupPaths& cgpaths) {
  if (!cgpaths.haveSubsystem(kSubSystemCpuSet)) {
    return folly::none;
  }

  auto partials = partialPaths(cgpaths.slice_);
  auto base = cgpaths.rootDir(kSubSystemCpuSet);
  for (auto rit = partials.rbegin(); rit != partials.rend(); ++rit) {
    auto cpu_cores_str = valueFromFile<std::string>(base / *rit / kCpuSetFile);
    if (!cpu_cores_str.empty()) {
      return parseIntSet(cpu_cores_str).size();
    }  // Else: Look higher up the tree.
  }
  throw BistroException("No `cpuset` cgroup had specified any CPUs");
}

folly::Optional<double> taskMemoryMB(const CGroupPaths& cgpaths) {
  if (!cgpaths.haveSubsystem(kSubSystemMemory)) {
    return folly::none;
  }
  return valueFromFile<uint64_t>(
    cgpaths.taskDir(kSubSystemMemory) / kMemoryUsageFile
  ) / (1024. * 1024.);
}

folly::Optional<uint64_t> taskCpuTimeMs(const CGroupPaths& cgpaths) {
  if (!cgpaths.haveSubsystem(kSubSystemCpuAcct)) {
    return folly::none;
  }
  // uint64_t nanoseconds overflow every 585 CPU-years (14 on a 40 core box).
  return valueFromFile<uint64_t>(
    cgpaths.taskDir(kSubSystemCpuAcct) / kCpuUsageFile
  ) / 1000000;  // ns => ms, we will not need finer-grained accounting
}

std::unordered_set<pid_t> taskPids(const CGroupPaths& cgpaths) {
  if (cgpaths.subsystems_.empty()) {
    return std::unordered_set<pid_t>{};
  }
  // Ask one of the tasks's subsystems for the tasks's PIDs.
  return folly::gen::byLine(folly::File((
      cgpaths.taskDir(*cgpaths.subsystems_.begin()) / kProcsFile
    ).native()))
    | folly::gen::eachTo<pid_t>()
    // Must dedupe, since `cgroup.procs` isn't guaranteed to have unique IDs.
    | folly::gen::as<std::unordered_set<pid_t>>();
}

}}}  // namespace facebook::bistro::cgroups
