/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncSignalHandler.h>
#include <memory>
#include <vector>

#include "bistro/bistro/worker/BistroWorkerHandler.h"

namespace facebook { namespace bistro {

class StopWorkerOnSignal : public folly::AsyncSignalHandler {
public:
  StopWorkerOnSignal(
    folly::EventBase* evb,
    std::vector<int> signals,
    std::shared_ptr<BistroWorkerHandler> worker
  ) : folly::AsyncSignalHandler(evb), worker_(std::move(worker)) {
    for (int sig : signals) {
      registerSignalHandler(sig);
    }
  }

  void signalReceived(int sig) noexcept override {
    LOG(ERROR) << "Got signal " << sig << ", shutting down worker.";
    worker_->killTasksAndStop();
  }

private:
  std::shared_ptr<BistroWorkerHandler> worker_;
};

}}  // namespace facebook::bistro
