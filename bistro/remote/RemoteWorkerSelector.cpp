/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "RemoteWorkerSelector.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/remote/RoundRobinRemoteWorkerSelector.h"
#include "bistro/bistro/remote/BusiestRemoteWorkerSelector.h"
#include "bistro/bistro/utils/EnumHash.h"

namespace facebook { namespace bistro {

namespace {
  typedef std::shared_ptr<RemoteWorkerSelector> Ptr;
  std::unordered_map<RemoteWorkerSelectorType, Ptr, EnumHash> policies = {
    {
      RemoteWorkerSelectorType::RoundRobin,
      Ptr(new RoundRobinRemoteWorkerSelector())
    },
    {
      RemoteWorkerSelectorType::Busiest,
      Ptr(new BusiestRemoteWorkerSelector())
    }
  };
}

RemoteWorkerSelector::~RemoteWorkerSelector() {}

RemoteWorkerSelector* RemoteWorkerSelector::getSingleton(
    RemoteWorkerSelectorType type) {

  return policies.find(type)->second.get();
}

bool RemoteWorkerSelector::jobCanRunOnWorker(
    const Job& job,
    const cpp2::BistroWorker& worker,
    int worker_level) noexcept {

  CHECK(worker_level < job.filters().size());
  return job.filters()[worker_level].doesPass(
    job.name(), worker.machineLock.hostname
  );
}

}}  // namespace facebook::bistro
