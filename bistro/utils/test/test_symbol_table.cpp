/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/utils/SymbolTable.h"

using namespace std;
using namespace facebook::bistro;

TEST(TestSymbolTable, HandleLookup) {
  StringTable t;

  EXPECT_EQ(0, t.size());
  EXPECT_EQ(0, t.insert("cat"));
  EXPECT_EQ(1, t.size());
  EXPECT_EQ(1, t.insert("dog"));
  EXPECT_EQ(2, t.size());
  EXPECT_EQ(2, t.insert("cow"));
  EXPECT_EQ(3, t.size());

  EXPECT_EQ(0, t.lookup("cat"));
  EXPECT_EQ(1, t.lookup("dog"));
  EXPECT_EQ(2, t.lookup("cow"));

  EXPECT_EQ(0, t.insert("cat"));
  EXPECT_EQ(1, t.insert("dog"));
  EXPECT_EQ(2, t.insert("cow"));

  EXPECT_EQ("cat", t.lookup(0));
  EXPECT_EQ("dog", t.lookup(1));
  EXPECT_EQ("cow", t.lookup(2));


  EXPECT_EQ((vector<string>{"cat", "dog", "cow"}), t.all());
}

TEST(TestSymbolTable, HandleNotFound) {
  StringTable t{"cat", "dog", "cow"};
  EXPECT_EQ(StringTable::NotFound, t.lookup("unknown"));
  EXPECT_NE(StringTable::NotFound, t.lookup("cat"));
}
