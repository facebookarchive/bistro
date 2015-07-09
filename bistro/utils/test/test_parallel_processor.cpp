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

#include <atomic>

#include "bistro/bistro/utils/ParallelProcessor.h"
#include <folly/Synchronized.h>

using namespace std;
using namespace facebook::bistro;

unique_ptr<int> dummyInt() {
  return unique_ptr<int>(new int(1));
}

TEST(TestParallelProcessor, HandleNoOp) {
  ParallelProcessor<int, int> p;
  vector<int> v(9331);
  for (int i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  p.run(v, dummyInt, [](int& val, int& ignore){});
  for (int i = 0; i < v.size(); ++i) {
    EXPECT_EQ(i, v[i]);
  }
}

TEST(TestParallelProcessor, HandleSimpleModify) {
  ParallelProcessor<int, int> p;
  vector<int> v(9331);
  for (int i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  p.run(v, dummyInt, [](int& val, int& ignore){ ++val; });
  for (int i = 0; i < v.size(); ++i) {
    EXPECT_EQ(i + 1, v[i]);
  }
}

TEST(TestParallelProcessor, HandleDifferentOptions) {
  for (int num_threads = 2; num_threads < 30; num_threads += 3) {
    for (int v_size = 1; v_size <= 500; v_size += 7) {
      ParallelProcessor<int, int> p(num_threads);
      vector<int> v(v_size);
      for (int i = 0; i < v.size(); ++i) {
        v[i] = i;
      }
      p.run(v, dummyInt, [](int& val, int& ignore){ ++val; });
      for (int i = 0; i < v.size(); ++i) {
        EXPECT_EQ(i + 1, v[i]);
      }
    }
  }
}

TEST(TestParallelProcessor, HandleSharedState) {
  ParallelProcessor<int, int> p;
  vector<int> v(9331);
  folly::Synchronized<vector<int>> out;
  for (int i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  p.run(v, dummyInt, [&out](int& val, int& ignore){ out->push_back(val); });
  SYNCHRONIZED(out) {
    EXPECT_EQ(out.size(), v.size());
    sort(out.begin(), out.end());
    for (int i = 0; i < v.size(); ++i) {
      EXPECT_EQ(out[i], v[i]);
    }
  }
}

TEST(TestParallelProcessor, HandleException) {
  ParallelProcessor<int, int> p(25, 0, chrono::seconds(0), false);
  vector<int> v(9331);
  for (int i = 0; i < v.size(); ++i) {
    v[i] = i;
  }
  p.run(v, dummyInt, [](int& val, int& ignore){
    if (val == 75) {
      throw runtime_error("fail whale");
    }
    ++val;
  });
  for (int i = 0; i < v.size(); ++i) {
    if (i == 75) {
      EXPECT_EQ(i, v[i]);
    } else {
      EXPECT_EQ(i + 1, v[i]);
    }
  }
}

TEST(TestParallelProcessor, HandleRetry) {
  ParallelProcessor<int, int> p(25, 4, chrono::seconds(0), false);
  vector<int> v(9331);
  for (int i = 0; i < v.size(); ++i) {
    v[i] = 3;
  }
  p.run(v, dummyInt, [](int& val, int& ignore){
    --val;
    if (val > 0) {
      throw runtime_error("fail whale");
    }
  });
  for (int i = 0; i < v.size(); ++i) {
    EXPECT_EQ(0, v[i]);
  }
}

TEST(TestParallelProcessor, HandleInitData) {
  atomic<int> initer_count(0);
  auto initer = [&initer_count]() {
    ++initer_count;
    return unique_ptr<int>(new int(7));
  };
  vector<int> v = { 1, 2, 3, 4, 5 };
  ParallelProcessor<int, int> p;
  p.run(
    v,
    initer,
    [](int& val, int& inited_value) {
      EXPECT_EQ(inited_value, 7);
    }
  );
  EXPECT_EQ(v.size(), initer_count.load());
}
