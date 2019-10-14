/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <vector>

#include "bistro/bistro/utils/hostname.h"

using namespace facebook::bistro;
using namespace std;

TEST(TestUtils, HandleStartsWithAny) {
  vector<string> v = { "foo", "bar" };
  EXPECT_TRUE(startsWithAny("foo123", v));
  EXPECT_TRUE(startsWithAny("bar123", v));
  EXPECT_FALSE(startsWithAny("moooo", v));
  EXPECT_FALSE(startsWithAny("foo", vector<string>{}));
}
