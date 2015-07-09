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

#include "bistro/bistro/utils/BackgroundThreadMixin.h"

using namespace std;
using namespace facebook::bistro;

struct Foo : BackgroundThreadMixin {
  explicit Foo(std::atomic<int>& d) : data(d) {}
  ~Foo() override { stopBackgroundThreads(); }

  void start() {
    runInBackgroundLoop([this](){
      ++data;
      return std::chrono::seconds(0);
    });
  }

  std::atomic<int>& data;
};

struct FooLongSleep : BackgroundThreadMixin {
  explicit FooLongSleep(std::atomic<int>& d) : data(d) {}
  ~FooLongSleep() override {
    stopBackgroundThreads();
    data.store(-1);
  }

  void start() {
    runInBackgroundLoop([this]() {
      data.store(1);
      return std::chrono::seconds(300);
    });
  }

  std::atomic<int>& data;
};

TEST(TestBackgroundThread, HandleBackgroundLoop) {
  std::atomic<int> data(0);
  {
    Foo f(data);
    EXPECT_EQ(0, data.load());
    f.start(); // Runs increment thread in background
    this_thread::sleep_for(chrono::milliseconds(10));
  }
  int val = data.load();
  EXPECT_GT(val, 0);
  // The increment thread should have been destroyed
  this_thread::sleep_for(chrono::milliseconds(10));
  int new_val = data.load();
}

TEST(TestBackgroundThread, HandleLongSleepingThread) {
  std::atomic<int> data(0);
  {
    FooLongSleep f(data);
    EXPECT_EQ(0, data.load());
    f.start();
    this_thread::sleep_for(chrono::milliseconds(10));
    EXPECT_EQ(1, data.load());
  }
  // Foo should have been destroyed, which stopped the thread!
  EXPECT_EQ(-1, data.load());
}
