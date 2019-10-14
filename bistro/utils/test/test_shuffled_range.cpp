/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <map>

#include "bistro/bistro/utils/ShuffledRange.h"

using namespace std;
using namespace facebook::bistro;

TEST(TestRandomIterator, TestVectorByIt) {
  vector<int> v(17);
  iota(v.begin(), v.end(), 6);
  vector<int> w;
  for (int i : shuffled(v.begin(), v.end())) {
    w.push_back(i);
  }
  EXPECT_NE(v, w); // unlikely
  sort(w.begin(), w.end());
  EXPECT_EQ(v, w);
}

TEST(TestRandomIterator, TestVector) {
  vector<int> v(17);
  iota(v.begin(), v.end(), 6);
  vector<int> w;
  for (int i : shuffled(v)) {
    w.push_back(i);
  }
  EXPECT_NE(v, w); // unlikely
  sort(w.begin(), w.end());
  EXPECT_EQ(v, w);
}

TEST(TestRandomIterator, TestVectorConst) {
  vector<int> c(17);
  iota(c.begin(), c.end(), 6);
  const vector<int> v(c);
  vector<int> w;
  for (int i : shuffled(v)) {
    w.push_back(i);
  }
  EXPECT_NE(v, w); // unlikely
  sort(w.begin(), w.end());
  EXPECT_EQ(v, w);
}

TEST(TestRandomIterator, TestMap) {
  map<string, int> m;
  vector<pair<string, int>> v;
  for (int i = 0; i < 50; ++i) {
    string s = "str_";
    s[3] = static_cast<char>(i);
    m[s] = i;
    v.push_back(make_pair(s, i));
  }

  map<string, int> m2;
  vector<pair<string, int>> w;
  for (auto p : shuffled(m.begin(), m.end())) {
    m2.insert(p);
    w.push_back(p);
  }
  EXPECT_NE(v, w); // unlikely
  EXPECT_EQ(m, m2); // maps are sorted !
  sort(w.begin(), w.end());
  EXPECT_EQ(v, w);
}

TEST(TestRandomIterator, TestModify) {
  vector<int> v{1, 2, 3};
  vector<int> w{4, 4, 4};

  for (int& i : shuffled(v)) {
    i = 4;
  }
  EXPECT_EQ(w, v);
}
