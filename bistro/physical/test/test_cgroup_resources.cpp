/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/experimental/TestUtil.h>
#include <gtest/gtest.h>

#include "bistro/bistro/physical/test/utils.h"
#include "bistro/bistro/physical/CGroupResources.h"

using namespace facebook::bistro;
using namespace facebook::bistro::cgroups;

struct TestCGroupResources : public ::testing::Test {
  TestCGroupResources() {
    cgopts.root = ".";
    cgopts.slice = "sl/ice";
  }

  // Our NUMA node descriptions are rooted at ".".
  CGroupPaths slicePaths() { return CGroupPaths(cgopts, folly::none, "."); }
  CGroupPaths taskPaths() { return CGroupPaths(cgopts, fs::path("task")); }

  folly::test::ChangeToTempDir td;
  cpp2::CGroupOptions cgopts;
};

TEST_F(TestCGroupResources, UsableNumaMemory) {
  // "sl" will define our NUMA nodes. Don't add a file to root, since we
  // should not be trying to read it.
  writeFilesToHierarchy("cpuset/sl/ice", "cpuset.mems", {{""}, {"0-1"}});

  constexpr uint64_t kNode0MB = 9234;
  constexpr uint64_t kNode1MB = 542;

  // Make the memory nodes' descriptions.
  writeNumaMeminfo("node0", kNode0MB);
  writeNumaMeminfo("node1", kNode1MB);

  // No subsystem, no result.
  EXPECT_FALSE(usableNumaMemoryMB(slicePaths()).has_value());
  // Confirm we read cpuset.mems & nodes correctly.
  cgopts.subsystems = {"cpuset"};
  EXPECT_EQ(kNode0MB + kNode1MB, *usableNumaMemoryMB(slicePaths()));
}

TEST_F(TestCGroupResources, UsableMemoryLimit) {
  // No subsystem, no limit.
  EXPECT_FALSE(usableMemoryLimitMB(slicePaths()).has_value());

  cgopts.subsystems = {"memory"};
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.limit_in_bytes", {strMB(7), strMB(6), strMB(5)}
  );

  // No hierarchy, no limit.
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.use_hierarchy", {{"0"}, {"0"}, {"0"}}
  );
  EXPECT_FALSE(usableMemoryLimitMB(slicePaths()).has_value());

  writeFilesToHierarchy(
    "memory/sl/ice", "memory.use_hierarchy", {{"1"}, {"0"}, {"0"}}
  );
  EXPECT_EQ(7., *usableMemoryLimitMB(slicePaths()));

  // Hierarchy makes us take min() over more values.
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.use_hierarchy", {{"0"}, {"1"}, {"0"}}
  );
  EXPECT_EQ(6., *usableMemoryLimitMB(slicePaths()));
}

TEST_F(TestCGroupResources, UsableCpuCores) {
  // No subsystem, no result.
  EXPECT_FALSE(usableCpuCores(slicePaths()).has_value());

  // Confirm that we only search as high up as needed to find a value.
  cgopts.subsystems = {"cpuset"};
  writeFilesToHierarchy("cpuset/sl/ice", "cpuset.cpus", {{"0,2-4"}});
  EXPECT_EQ(4, *usableCpuCores(slicePaths()));
  writeFilesToHierarchy("cpuset/sl/ice", "cpuset.cpus", {{""}, {"5,9"}});
  EXPECT_EQ(2, *usableCpuCores(slicePaths()));
  writeFilesToHierarchy("cpuset/sl/ice", "cpuset.cpus", {{""}, {""}, {"31"}});
  EXPECT_EQ(1, *usableCpuCores(slicePaths()));
}

TEST_F(TestCGroupResources, TaskMemory) {
  // No subsystem, no result.
  EXPECT_FALSE(taskMemoryMB(slicePaths()).has_value());

  cgopts.subsystems = {"memory"};
  writeFilesToHierarchy(
    "memory/sl/ice/task", "memory.usage_in_bytes", {strMB(37)}
  );
  EXPECT_EQ(37., *taskMemoryMB(taskPaths()));
}

TEST_F(TestCGroupResources, TaskCpuTime) {
  // No subsystem, no result.
  EXPECT_FALSE(taskCpuTimeMs(slicePaths()).has_value());

  cgopts.subsystems = {"cpuacct"};
  writeFilesToHierarchy("cpuacct/sl/ice/task", "cpuacct.usage", {{"7000000"}});
  EXPECT_EQ(7, *taskCpuTimeMs(taskPaths()));
}

TEST_F(TestCGroupResources, TaskPids) {
  // No subsystem, no result.
  EXPECT_TRUE(taskPids(slicePaths()).empty());

  cgopts.subsystems = {"bork"};  // Any subsystem is fine
  writeFilesToHierarchy("bork/sl/ice/task", "cgroup.procs", {{"1\n2\n1\n3"}});
  EXPECT_EQ((std::unordered_set<pid_t>{1, 2, 3}), taskPids(taskPaths()));
}
