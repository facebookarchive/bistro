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

#include "bistro/bistro/config/utils.h"

using namespace facebook::bistro;
using namespace folly;

TEST(TestUtils, HandleUpdateDynamic) {
  dynamic d1 = dynamic::object
    ("cat", 1)
    ("dog", 2)
    ("foo", "bar")
  ;
  dynamic d2 = dynamic::object
    ("bar", 123)
    ("foo", 5)
  ;

  dynamic d3 = merge(d1, d2);
  EXPECT_EQ(4, d3.size());
  EXPECT_EQ(1, d3["cat"].asInt());
  EXPECT_EQ(2, d3["dog"].asInt());
  EXPECT_EQ(123, d3["bar"].asInt());
  EXPECT_EQ(5, d3["foo"].asInt());

  update(d1, d2);
  EXPECT_EQ(d1, d3);
}
