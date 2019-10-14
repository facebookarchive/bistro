/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/io/async/EventBaseManager.h>

#include "bistro/bistro/server/test/ThriftMonitorTestThread.h"
#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"

using namespace facebook::bistro;

TEST(TestThriftMonitor, HandleNew) {
  ThriftMonitorTestThread tm;
  EXPECT_EQ(
    facebook::fb303::cpp2::fb_status::ALIVE,
    tm.getClient(
      folly::EventBaseManager::get()->getEventBase()
    )->sync_getStatus()
  );
}
