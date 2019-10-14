/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Subprocess.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

/**
 * Make a new cgroup directory in each of the specified subsystems.  Set
 * specified resource limits.  Return a folly::Subprocess-compatible list of
 * paths to the cgroups' "/cgroup.procs" files.
 *
 * On error, remove all just-created cgroup directories (to avoid leaking)
 * and throw.
 *
 * NB It is an error if any of the cgroups already exists.
 */
std::vector<std::string> cgroupSetup(
  const std::string& cgname,
  const cpp2::CGroupOptions& cgopts
);

/**
 * Place the child into the given Linux cgroups before starting it.  The
 * paths must point to the cgroups' `/cgroup.procs` or `/tasks` files to
 * modify.  The subprocess will start only if all placements succeed.  If
 * any of the cgroup placements fail, the remaining placements will still be
 * tried, ensuring that any brand-new cgroups made just for this process
 * will be immediately eligible for "notify_on_release", and will not leak.
 *
 * CGroup placement must occur between fork() and exec(), or the exec()ed
 * code could race to create processes outside the cgroup.
 *
 * Hints on creating / maintaining cgroups for use with this facility:
 *
 *  - Consider using your deployment automation to set up your host's
 *      /sys/fs/cgroup/{cpu,cpuacct,memory,...}/release_agent
 *    with an auto-cleanup script, e.g.:
 *      #!/bin/sh
 *      /bin/logger "Removing memory cgroup: $@"
 *      sudo /bin/rmdir -p "/sys/fs/cgroup/memory/$1"
 *
 *  - You will need access to /sys/fs/cgroup/{cpu,memory,...} either
 *    via `chmod a+wt` or via a run-as-root service, so that you can create
 *    and configure the cgroups' directories.  Remember to set
 *    `notify_on_release` to 1 in your cgroup to enable auto-delete.
 *
 * == BEFORE MODIFYING THIS CODE ==
 *
 * **Please** read the warnings in folly/Subprocess.h or in CGroupSetup.cpp.
 */
class AddChildToCGroups
  : public folly::Subprocess::DangerousPostForkPreExecCallback {
public:
  explicit AddChildToCGroups(std::vector<std::string> procs_paths)
    : cgroupProcsPaths_(std::move(procs_paths)) {}
  ~AddChildToCGroups() override {}
  int operator()() override; // Return 0 on success, or an `errno` error code.
 private:
  // It is crucial that this be `const`, since otherwise we could not safely
  // use `c_str()` on the strings after `vfork()`.
  const std::vector<std::string> cgroupProcsPaths_;
};

// Files
constexpr folly::StringPiece kCGroupProcs = "cgroup.procs";
constexpr folly::StringPiece kNotifyOnRelease = "notify_on_release";
constexpr folly::StringPiece kCPUShares = "cpu.shares";
constexpr folly::StringPiece kMemoryLimitInBytes = "memory.limit_in_bytes";
// Subsystems
constexpr folly::StringPiece kMemory = "memory";
constexpr folly::StringPiece kCPU = "cpu";

}}  // namespace facebook::bistro
