/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/experimental/TestUtil.h>
#include <gtest/gtest.h>

#include "bistro/bistro/physical/test/utils.h"
#include "bistro/bistro/physical/UsablePhysicalResourceFetcher.h"

DECLARE_string(nvidia_smi);

using namespace facebook::bistro;

struct TestUsablePhysicalResourceFetcher : public ::testing::Test {
  TestUsablePhysicalResourceFetcher() {
    cgopts.root = ".";
    cgopts.slice = "sl/ice";
  }
  UsablePhysicalResourceFetcher fetcher() {
    return UsablePhysicalResourceFetcher(
      CGroupPaths(cgopts, folly::none, ".")  // Numa nodes rooted at "."
    );
  }

  folly::test::ChangeToTempDir td;
  cpp2::CGroupOptions cgopts;
};

TEST_F(TestUsablePhysicalResourceFetcher, Memory) {
  // No CGroups, system RAM fallback
  auto sys_mb = fetcher().memoryMB();
  EXPECT_LT(3, sys_mb);

  // The memory limit is only effective when it's less than available RAM.
  uint64_t limit_mb = sys_mb - 1;  // Truncate the double
  cgopts.subsystems.emplace_back("memory");
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.use_hierarchy", {{"1"}, {"0"}, {"0"}}
  );
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.limit_in_bytes", {strMB(limit_mb)}
  );
  EXPECT_EQ(limit_mb, fetcher().memoryMB());
  EXPECT_GT(limit_mb + 2, sys_mb);
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.limit_in_bytes", {strMB(limit_mb + 2)}
  );
  EXPECT_EQ(sys_mb, fetcher().memoryMB());

  // When `cpuset` NUMA memory constraints are set, ignore system RAM.
  uint64_t numa_mb = sys_mb - 1;  // Truncate the double
  cgopts.subsystems.emplace_back("cpuset");
  writeNumaMeminfo("node0", numa_mb);
  writeFilesToHierarchy("cpuset/sl/ice", "cpuset.mems", {{""}, {""}, {"0"}});
  EXPECT_EQ(numa_mb, fetcher().memoryMB());

  // The memory limit restricts NUMA capacity, too.
  EXPECT_LT(limit_mb - 1, numa_mb);
  writeFilesToHierarchy(
    "memory/sl/ice", "memory.limit_in_bytes", {strMB(limit_mb - 1)}
  );
  EXPECT_EQ(limit_mb - 1, fetcher().memoryMB());
}

TEST_F(TestUsablePhysicalResourceFetcher, CpuCores) {
  // No CGroups, system fallback
  auto sys_cores = fetcher().cpuCores();
  EXPECT_LE(1, sys_cores);

  // `cpuset` takes precedence if available.
  cgopts.subsystems = {"cpuset"};
  writeFilesToHierarchy(  // A range with 1 more core
    "cpuset/sl/ice", "cpuset.cpus", {folly::to<std::string>("0-", sys_cores)}
  );
  EXPECT_EQ(sys_cores + 1, fetcher().cpuCores());
}

TEST_F(TestUsablePhysicalResourceFetcher, Gpus) {
  // No `nvidia-smi`, no GPUs.
  FLAGS_nvidia_smi = "";
  EXPECT_TRUE(fetcher().gpus(1000).empty());

  makeShellScript(
    "a",
    "test $# = 2 -a $1 = --format=csv,noheader,nounits "
      "-a $2 = --query-gpu=gpu_bus_id,memory.total,name || exit 37; "
    "echo '0000:28:00.0, 11519, Tesla K40m'; "
    "echo '0000:0E:00.0, 2287, Quadro M6000'; "
  );
  FLAGS_nvidia_smi = "a";
  std::vector<cpp2::GPUInfo> gpus;
  gpus.emplace_back();
  gpus.back().pciBusID = "0000:28:00.0";
  gpus.back().name = "Tesla K40m";
  gpus.back().memoryMB = 11519;
  gpus.back().compute = 1.0;
  gpus.emplace_back();
  gpus.back().pciBusID = "0000:0E:00.0";
  gpus.back().name = "Quadro M6000";
  gpus.back().memoryMB = 2287;
  gpus.back().compute = 1.0;
  EXPECT_EQ(gpus, fetcher().gpus(5000));  // 5s timeout to avoid flaky tests.
}
