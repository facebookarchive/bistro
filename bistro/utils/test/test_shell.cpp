/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include "bistro/bistro/utils/shell.h"

#include <folly/Subprocess.h>

using facebook::bistro::escapeShellArgsInsecure;

void check(const std::vector<std::string>& args) {
  for (size_t i = 0; i < args.size(); ++i) {
    folly::Subprocess p({"/bin/sh", "-c", folly::to<std::string>(
      "/bin/sh -c 'echo -n $", i, "' ", escapeShellArgsInsecure(args)
    )}, folly::Subprocess::Options().pipeStdout());
    EXPECT_EQ(args[i], p.communicate().first);
    p.wait();
  }
}

TEST(TestShell, All) {
  check({"\\'foo\\'"});
  check({"\""});
  check({"'","\\'", "'\\", "\\", "\"'\"", "'\\\"'", "\"\\'\""});
  check({"$", "\\\'>X", "\'|"});
}
