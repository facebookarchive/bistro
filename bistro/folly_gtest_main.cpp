/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <glog/logging.h>
#include "gtest/gtest.h"

int main(int argc, char **argv) {
  FLAGS_logtostderr = 1;
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  return RUN_ALL_TESTS();
}
