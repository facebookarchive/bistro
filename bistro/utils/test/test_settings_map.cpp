/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/utils/SettingsMap.h"
#include <folly/dynamic.h>

using namespace std;
using namespace facebook::bistro;

TEST(TestSettingsMap, HandleAll) {
  SettingsMap t;
  t.set("foo", "kittens");
  t.set("object", folly::dynamic::object("test", "ok"));

  EXPECT_TRUE(t.get("foo").isString());
  EXPECT_TRUE(t.get("invalid_key").empty());
  EXPECT_EQ(2, t.get("invalid_key", folly::dynamic::array(1, 2)).size());
  EXPECT_THROW(t.require("invalid_key"), runtime_error);

  EXPECT_TRUE(t.get("object").isObject());
  EXPECT_EQ(t.require("object")["test"], "ok");
}

TEST(TestSettingsMap, HandleCreateFromDynamic) {
  folly::dynamic d = folly::dynamic::object
    ("foo", "kittens")
    ("bar", "moo")
  ;

  SettingsMap t(d);
  EXPECT_EQ("kittens", t.convert<string>("foo"));
  EXPECT_EQ("moo", t.convert<string>("bar"));
}

TEST(TestSettingsMap, HandleConvert) {
  SettingsMap t;
  t.set("foo", "kittens");
  t.set("bar", "moo");
  t.set("int", "5");
  t.set("double", 5.5);
  t.set("true", true);
  t.set("false", "false");

  EXPECT_EQ("kittens", t.convert<string>("foo"));
  EXPECT_EQ("moo", t.convert<string>("bar"));
  EXPECT_EQ("default", t.convert<string>("invalid_key", "default"));
  EXPECT_EQ(5, t.convert<int>("int"));
  EXPECT_EQ(5.5, t.convert<double>("double"));
  EXPECT_EQ(77, t.convert<int>("missing", 77));
  EXPECT_TRUE(t.convert<bool>("true"));
  EXPECT_FALSE(t.convert<bool>("false"));
}
