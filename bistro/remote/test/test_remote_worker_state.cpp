/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/remote/RemoteWorkerState.h"

using namespace facebook::bistro;
using State = RemoteWorkerState::State;

TEST(TestRemoteWorkerState, HandleAll) {
  RemoteWorkerState s(0);
  EXPECT_EQ(State::NEW, s.state_);
  EXPECT_EQ(
    std::make_pair(State::NEW, false), s.computeState(0, 0, 0, 1, false)
  );

  s.state_ = State::UNHEALTHY;
  s.timeLastGoodHealthcheckSent_ = 1;
  s.timeLastHeartbeatReceived_ = 2;

  // Unhealthy because hasBeenHealthy_ is false, and there's no override
  EXPECT_EQ(
    std::make_pair(State::UNHEALTHY, true), s.computeState(3, 5, 5, 3, false)
  );
  // Been unhealthy for 3 ticks, but should get lost after 1. However, we
  // will not lose the worker since it is ONLY blocked from becoming healthy
  // by not being part of the worker set consensus.
  EXPECT_EQ(  // Compare to the "hasBeenHealthy_=true" variant below.
    std::make_pair(State::UNHEALTHY, true), s.computeState(5, 5, 5, 1, false)
  );
  EXPECT_EQ(  // Die since we're unhealthy & not blocked by consensus
    std::make_pair(State::MUST_DIE, false), s.computeState(5, 5, 2, 1, true)
  );

  // Allow the worker to be healthy
  EXPECT_EQ(
    std::make_pair(State::HEALTHY, false), s.computeState(3, 5, 5, 1, true)
  );

  // Now remove the barrier to the worker becoming healthy on its own.
  s.hasBeenHealthy_ = true;

  EXPECT_EQ(
    std::make_pair(State::HEALTHY, false), s.computeState(3, 5, 5, 1, false)
  );
  s.state_ = State::HEALTHY;
  // Some time passes without a heartbeat, and we're unhealthy.
  EXPECT_EQ(
    std::make_pair(State::UNHEALTHY, false), s.computeState(5, 5, 2, 1, false)
  );

  s.state_ = State::UNHEALTHY;
  EXPECT_EQ(
    std::make_pair(State::HEALTHY, false), s.computeState(3, 5, 5, 1, false)
  );
  EXPECT_EQ(
    std::make_pair(State::UNHEALTHY, false), s.computeState(5, 5, 2, 10, false)
  );
  EXPECT_EQ(  // Compare to the "hasBeenHealthy_=false" variant above.
    std::make_pair(State::MUST_DIE, false), s.computeState(5, 5, 2, 1, false)
  );

  // Cannot leave MUST_DIE, though otherwise healthy
  EXPECT_EQ(
    std::make_pair(State::HEALTHY, false), s.computeState(5, 9, 9, 9, false)
  );
  s.state_ = State::MUST_DIE;
  EXPECT_EQ(
    std::make_pair(State::MUST_DIE, false), s.computeState(5, 9, 9, 9, false)
  );
}
