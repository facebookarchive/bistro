/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "folly/experimental/ThreadedRepeatingFunctionRunner.h"

namespace facebook { namespace bistro {

/**
 * DEPRECATED -- instead, include ThreadedRepeatingFunctionRunner by
 * composition.  Also, be careful not to pass "this" to the thread from your
 * constructor.
 *
 * A mixin that supports having one or more 'background' threads to do some
 * work. The most common pattern looks like this:
 * struct MyClass : BackgroundThreadMixin {
 *   MyClass() {
 *     runInBackgroundLoop(bind(&MyClass::foo, this));
 *   }
 *   ~MyClass() {
 *     stopBackgroundThreads();
 *   }
 * }
 */
class BackgroundThreadMixin {

protected:
  virtual ~BackgroundThreadMixin() {}

  /**
   * Run a function in a background loop, sleeping the returned time between
   * calls. Optionally sleep a given amount of time before the first call.
   */
  void runInBackgroundLoop(
    std::function<std::chrono::milliseconds()> f,
    std::chrono::milliseconds initialSleepAmount = std::chrono::milliseconds(0)
  ) {
    threads_.add(f, initialSleepAmount);
  }

  void stopBackgroundThreads() {
    threads_.stop();
  }

private:
  folly::ThreadedRepeatingFunctionRunner threads_;
};

}}
