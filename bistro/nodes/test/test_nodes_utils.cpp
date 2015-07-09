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
