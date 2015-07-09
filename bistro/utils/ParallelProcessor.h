/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <chrono>
#include <functional>
#include <glog/logging.h>
#include <thread>
#include <vector>

namespace facebook { namespace bistro {

template<
  class Item,
  class InitData,
  class InitDataPtr = std::unique_ptr<InitData>,
  class Container = std::vector<Item>
>
class ParallelProcessor {

public:
  typedef std::function<InitDataPtr()> Initializer;
  typedef std::function<void(Item&, InitData&)> Callback;

  explicit ParallelProcessor(
    const int num_threads = std::thread::hardware_concurrency(),
    const int max_retries = 0,
    const std::chrono::seconds sleep_duration = std::chrono::seconds(5),
    bool log_error = true)
    : numThreads_(num_threads == 0 ? 20 : num_threads),
      maxTries_(max_retries + 1),
      sleepDuration_(sleep_duration),
      logError_(log_error) {
    CHECK(numThreads_ >= 1);
  }

  void run(Container& c, Initializer init, Callback cob) {
    std::vector<std::thread> threads;
    std::atomic<int> index(0);
    for (int i = 0; i < numThreads_; ++i) {
      threads.emplace_back([this, cob, init, &c, &index]() {
        for(int k = index.fetch_add(1); k < c.size(); k = index.fetch_add(1)) {
          int tries = maxTries_;
          while (tries--) {
            try {
              auto data_ptr(init());
              cob(c.at(k), *data_ptr);
              break;
            } catch (const std::exception& e) {
              if (logError_) {
                LOG(ERROR) << "Error running callback: " << e.what();
              }
              std::this_thread::sleep_for(sleepDuration_);
            }
          }
        }
      });
    }
    for (auto& t : threads) {
      t.join();
    }
  }

private:
  const int numThreads_;
  const int maxTries_;
  const std::chrono::seconds sleepDuration_;
  const bool logError_;

};

}}
