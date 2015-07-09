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

#include "bistro/bistro/remote/RemoteWorkerState.h"

using namespace facebook::bistro;

TEST(TestRemoteWorkerState, HandleAll) {
  RemoteWorkerState s(0);
  EXPECT_EQ(RemoteWorkerState::State::NEW, s.state_);
  EXPECT_EQ(RemoteWorkerState::State::NEW, s.computeState(0, 0, 0, 1));

  s.state_ = RemoteWorkerState::State::HEALTHY;
  s.timeLastGoodHealthcheckSent_ = 1;
  s.timeLastHeartbeatReceived_ = 2;
  EXPECT_EQ(RemoteWorkerState::State::HEALTHY, s.computeState(3, 5, 5, 1));
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, s.computeState(5, 5, 2, 1));

  s.state_ = RemoteWorkerState::State::UNHEALTHY;
  EXPECT_EQ(RemoteWorkerState::State::HEALTHY, s.computeState(3, 5, 5, 1));
  EXPECT_EQ(RemoteWorkerState::State::UNHEALTHY, s.computeState(5, 5, 2, 10));
  EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, s.computeState(5, 5, 2, 1));

  s.state_ = RemoteWorkerState::State::MUST_DIE;
  EXPECT_EQ(RemoteWorkerState::State::MUST_DIE, s.computeState(5, 10, 10, 10));
}
