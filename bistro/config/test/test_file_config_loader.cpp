/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <folly/Conv.h>
#include <folly/dynamic.h>
#include <fstream>

#include "bistro/bistro/config/FileConfigLoader.h"
#include "bistro/bistro/scheduler/SchedulerPolicyRegistry.h"
#include "bistro/bistro/utils/TemporaryFile.h"

using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestFileConfigLoader, HandleValidConfig) {
  registerSchedulerPolicy(kSchedulePolicyRankedPriority.str(), nullptr);

  TemporaryFile file;
  file.writeString(
    "{ \"bistro_settings\" : { "
      " \"enabled\" : true, "
      " \"working_wait\" : 0.5, "
      " \"nodes\" : { "
        " \"levels\" : [ \" foo \" ], "
        " \"node_source\" : \"range_label\""
      "},"
      " \"resources\" : {}, "
      " \"scheduler\" : \"ranked_priority\" "
    "}, \"bistro_job->test_job\" : {"
        "\"owner\" : \"test\" "
    "}}"
  );

  // Short update period in case we miss the first refresh
  FileConfigLoader loader(std::chrono::milliseconds(5), file.getFilename());
  auto c = loader.getDataOrThrow();
  EXPECT_TRUE(c->enabled);
  EXPECT_EQ(std::chrono::milliseconds(500), c->workingWait);
  EXPECT_EQ(1, c->jobs.size());
}
