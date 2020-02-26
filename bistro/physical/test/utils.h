/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/Optional.h>

namespace facebook { namespace bistro {

namespace fs = boost::filesystem;

void makeShellScript(boost::filesystem::path file, std::string content) {
  EXPECT_TRUE(folly::writeFile(folly::to<std::string>(
    "#!/bin/sh\n", content, "\n"
  ), file.c_str()));
  chmod(file.c_str(), S_IRUSR | S_IXUSR);
}

// Makes dir if necessary, writes values[0] to dir/filename, values[1] to
// dir.parent_path()/filename, etc.  Does not make folly::none-valued files.
void writeFilesToHierarchy(
    boost::filesystem::path dir,
    std::string filename,
    std::vector<folly::Optional<std::string>> values) {
  fs::create_directories(dir);
  auto dir_it = dir;
  for (const auto& val : values) {
    ASSERT_FALSE(dir_it.empty());  // Don't try to write to ".", that's a bug.
    if (val.has_value()) {
      EXPECT_TRUE(folly::writeFile(*val + "\n", (dir_it / filename).c_str()));
    }
    dir_it = dir_it.parent_path();
  }
  // dir_it may have leftovers if we are only populating the bottom few files.
}

std::string strMB(uint64_t n) {
  return folly::to<std::string>(n * (1LLU << 20));
}

void writeNumaMeminfo(const std::string& node, uint64_t mem_mb) {
  EXPECT_TRUE(fs::create_directory(node));
  auto f = node + "/meminfo";
  EXPECT_TRUE(folly::writeFile(folly::to<std::string>(
    "Node ", node.back(), " MemFree:        123456 kB\n"
    "Node ", node.back(), " MemUsed:        7890 kB\n",
    "Node ", node.back(), " MemTotal:       ", (1024 * mem_mb), " kB\n",
    "Node ", node.back(), " Active:         1234 kB\n",
    "Node ", node.back(), " Inactive:       567 kB\n",
    "Node ", node.back(), " Active(anon):   890 kB\n"
  ), f.c_str()));
}

}}  // namespace facebook::bistro
