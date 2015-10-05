/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>

#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/remote/RemoteWorkers.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"

using namespace facebook::bistro;
using namespace folly::test;

TEST(TestRemoteWorkers, TestProtocolMismatch) {
  cpp2::BistroWorker worker;
  worker.protocolVersion = -1;
  RemoteWorkerUpdate update;
  RemoteWorkers r;

  // Mismatched version
  EXPECT_THROW(r.processHeartbeat(&update, worker), std::runtime_error);
  EXPECT_TRUE(r.begin() == r.end());

  // Matched version
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  auto res = r.processHeartbeat(&update, worker);
  EXPECT_TRUE(res.hasValue());
  EXPECT_FALSE(r.begin() == r.end());
  EXPECT_TRUE(++r.begin() == r.end());
  EXPECT_NE(nullptr, r.getWorker(worker.shard));
}
