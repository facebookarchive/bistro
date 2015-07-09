/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/BackgroundThreads.h"

#include <glog/logging.h>
#include <iostream>

// This is mainly exposed so that I can set it way low in tests.
DEFINE_int32(
  incremental_sleep_ms, 1000,
  "For background threads, trade off how often they wake up vs how "
  "quickly they can be terminated."
);

namespace facebook { namespace bistro {

BackgroundThreads::BackgroundThreads() : keepRunning_(true) {
}

BackgroundThreads::~BackgroundThreads() {
  stopAndWarn("BackgroundThreads");
}

void BackgroundThreads::stopAndWarn(const std::string& class_of_destructor) {
  if (keepRunning_.load()) {
    LOG(ERROR)
      << "BackgroundThreads::stop() should already have been called, since "
      << "the " << class_of_destructor << " destructor is now running. This "
      << "is unsafe because it means that the background threads may be "
      << "accessing class state (e.g. derived class members, or members "
      << "that were declared below the BackgroundThreads) that was already "
      << "destroyed.";
    stop();
  }
}

void BackgroundThreads::stop() {
  if (keepRunning_.exchange(false)) {  // Do nothing if this is called twice.
    for (auto& t : threads_) {
      t.join();
    }
  }
}

void BackgroundThreads::add(
    std::function<std::chrono::milliseconds()> f,
    std::chrono::milliseconds amount) {
  threads_.emplace_back(&BackgroundThreads::executeInLoop, this, f, amount);
}

void BackgroundThreads::executeInLoop(
    std::function<std::chrono::milliseconds()> f,
    std::chrono::milliseconds initialSleepAmount) const {

  incrementalSleep(initialSleepAmount);
  while (keepRunning_.load()) {
    incrementalSleep(f());
  }
}

void BackgroundThreads::incrementalSleep(
    std::chrono::milliseconds amount) const {

  while (amount.count() > 0 && keepRunning_.load()) {
    auto s = amount > std::chrono::milliseconds(FLAGS_incremental_sleep_ms) ?
        std::chrono::milliseconds(FLAGS_incremental_sleep_ms) :
        amount;
    std::this_thread::sleep_for(s);
    amount -= s;
  }
}

}}
