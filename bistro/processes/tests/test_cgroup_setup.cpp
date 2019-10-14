/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/Conv.h>
#include <folly/experimental/TestUtil.h>
#include <folly/FileUtil.h>

#include "bistro/bistro/processes/CGroupSetup.h"

using namespace facebook::bistro;

TEST(TestCGroupSetup, TestAddChildToCGroups) {
  folly::test::ChangeToTempDir td;

  // CGroups paths must exist to be written to.
  ASSERT_TRUE(folly::writeFile(std::string(), "a"));
  ASSERT_TRUE(folly::writeFile(std::string(), "b"));

  // Try writing to the CGroups we just made.
  AddChildToCGroups proc1({"a", "b"});
  folly::Subprocess proc(
    std::vector<std::string>{"/bin/echo"},
    folly::Subprocess::Options().dangerousPostForkPreExecCallback(&proc1)
  );
  auto read_file_fn = [](const char* filename) {
    std::string s;
    EXPECT_TRUE(folly::readFile(filename, s));
    return s;  // We'll return "" on failure
  };
  EXPECT_EQ("0", read_file_fn("a"));
  EXPECT_EQ("0", read_file_fn("b"));
  proc.waitChecked();

  // Now try again, with a non-existent file mid-list, to test error handling
  const std::string kDoesNotExist = "does_not_exist";
  AddChildToCGroups proc2({"a", kDoesNotExist, "b"});
  EXPECT_THROW(
    folly::Subprocess(
      std::vector<std::string>{"/bin/echo"},
      folly::Subprocess::Options().dangerousPostForkPreExecCallback(&proc2)
    ),
    folly::SubprocessSpawnError
  );
  EXPECT_FALSE(boost::filesystem::exists(kDoesNotExist));
  // The contract is that these two get written, even if the middle one fails.
  EXPECT_EQ("00", read_file_fn("a"));
  EXPECT_EQ("00", read_file_fn("b"));
}

TEST(TestCGroupSetup, TestSetup) {
  folly::test::ChangeToTempDir td;

  // Nothing happens until we specify some subsystems.
  std::string name = "nested/name";
  cpp2::CGroupOptions opts;
  opts.unitTestCreateFiles = true;
  opts.root = "root";
  opts.slice = "slice";
  EXPECT_EQ(0, cgroupSetup(name, opts).size());
  EXPECT_TRUE(boost::filesystem::is_empty("."));

  // The slice directory must exist for this subsystem.
  opts.subsystems = {"sys1"};
  EXPECT_THROW([&]() {
    try { cgroupSetup(name, opts); } catch (const std::exception& ex) {
      EXPECT_PCRE_MATCH(".*root/subsystem/slice must be a dir.*", ex.what());
      throw;
    }
  }(), std::runtime_error);

  auto cg_dir_fn = [&](std::string subsystem) {
    return folly::to<std::string>("root/", subsystem, "/slice/", name);
  };

  auto check_subsystem_file_fn = [&](
    std::string subsystem, std::string filename, std::string expected
  ) {
    std::string s;
    auto path = cg_dir_fn(subsystem) + "/" + filename;
    EXPECT_TRUE(folly::readFile(path.c_str(), s));
    EXPECT_EQ(expected, s) << " in " << path;
  };

  auto check_cgroups_fn = [&]() {
    cgroupSetup(name, opts);
    for (const auto& subsystem : opts.subsystems) {
      // Empty file created when cgroupSetup checks if it is writable.
      check_subsystem_file_fn(subsystem, "cgroup.procs", "");
      check_subsystem_file_fn(subsystem, "notify_on_release", "1");
    }
  };

  auto clean_subsystem_fn = [&](std::string subsystem) {
    // Check that there are no other files left.
    auto d = cg_dir_fn(subsystem);
    EXPECT_TRUE(boost::filesystem::remove(d + "/cgroup.procs"));
    EXPECT_TRUE(boost::filesystem::remove(d + "/notify_on_release"));
    EXPECT_TRUE(boost::filesystem::is_empty(d));
    EXPECT_TRUE(boost::filesystem::remove(d));
  };

  // Once we make the slice directory, everything works.
  EXPECT_TRUE(boost::filesystem::create_directories("root/sys1/slice"));
  check_cgroups_fn();
  clean_subsystem_fn("sys1");

  // If the task's cgroup already exists, this will fail.
  auto cg1_dir = cg_dir_fn("sys1");
  EXPECT_TRUE(boost::filesystem::create_directory(cg1_dir));
  EXPECT_THROW([&]() {
    try { cgroupSetup(name, opts); } catch (const std::exception& ex) {
      EXPECT_PCRE_MATCH(folly::to<std::string>(
        ".*: CGroup ", cg1_dir, " already exists"
      ), ex.what());
      throw;
    }
  }(), std::runtime_error);

  // Remove the cgroup, and it works again.
  EXPECT_TRUE(boost::filesystem::remove(cg1_dir));
  check_cgroups_fn();
  clean_subsystem_fn("sys1");

  // Now check that when some the subsystems fail to create, we try to
  // remove any cgroups that were created successfully.
  opts.subsystems = {"sys2", "sys1", "sys3"};
  // sys2 will fail because the slice dir does not exist, but sys3 will fail
  // because the cgroup already exists -- and we will NOT try to remove it.
  auto cg3_dir = cg_dir_fn("sys3");
  EXPECT_TRUE(boost::filesystem::create_directories(cg3_dir));
  EXPECT_THROW([&]() {
    try { cgroupSetup(name, opts); } catch (const std::exception& ex) {
      EXPECT_PCRE_MATCH(folly::to<std::string>(
        "Failed to make cgroup directories: ",
        "CGroup root/subsystem/slice must be a directory: ",
        "root/sys2/slice: No such file or directory; ",
        "CGroup ", cg3_dir, " already exists; ",
        // If this were a real cgroup, it would have been removed.
        "Removing ", cg1_dir, ": Directory not empty"
      ), ex.what());
      throw;
    }
  }(), std::runtime_error);
  clean_subsystem_fn("sys1");  // Only needed since it's not a real cgroup.
  // The sys3 cgroup already existed, and was not removed.
  EXPECT_TRUE(boost::filesystem::is_directory(cg3_dir));
  EXPECT_TRUE(boost::filesystem::is_empty(cg3_dir));

  // Set some CPU and memory limits.
  opts.memoryLimitInBytes = 1337;
  opts.cpuShares = 3;
  opts.subsystems = {"memory", "cpu"};
  EXPECT_TRUE(boost::filesystem::create_directories("root/cpu/slice"));
  EXPECT_TRUE(boost::filesystem::create_directories("root/memory/slice"));
  check_cgroups_fn();

  check_subsystem_file_fn("cpu", "cpu.shares", "3");
  EXPECT_TRUE(boost::filesystem::remove(cg_dir_fn("cpu") + "/cpu.shares"));
  clean_subsystem_fn("cpu");

  check_subsystem_file_fn("memory", "memory.limit_in_bytes", "1337");
  EXPECT_TRUE(boost::filesystem::remove(
    cg_dir_fn("memory") + "/memory.limit_in_bytes"
  ));
  clean_subsystem_fn("memory");
}
