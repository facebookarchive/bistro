/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/remote/RemoteWorkers.h"

#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/remote/RemoteWorker.h"
#include "bistro/bistro/remote/RemoteWorkerUpdate.h"
#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

using namespace std;
using apache::thrift::debugString;

folly::Optional<cpp2::SchedulerHeartbeatResponse>
RemoteWorkers::processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& worker) {

  const auto& shard = worker.shard;
  auto worker_it = workerPool_.find(shard);
  // Make a RemoteWorker if the shard is new
  if (worker_it == workerPool_.end()) {
    // Even though we're adding another element, the "nextShard_"
    // round-robin iterators need not be updated, since new elements should
    // be distributed randomly throughout the hash tables.
    auto res = workerPool_.emplace(
      shard,
      std::make_shared<RemoteWorker>(update->curTime(), worker)
    );
    // Add the same pointer to the right host worker pool
    CHECK(getHostWorkerPool(worker.machineLock.hostname).emplace(
      shard, res.first->second
    ).second) << "Worker pool for hostname " << worker.machineLock.hostname
      << " already had " << " shard " << shard;
    return res.first->second->processHeartbeat(update, worker);
  }
  // If the hostname changed, move the worker to the new host pool
  const auto& old_hostname =
    worker_it->second->getBistroWorker().machineLock.hostname;
  const auto& new_hostname = worker.machineLock.hostname;
  if (new_hostname != old_hostname) {
    // This might "invalidate" the nextShard_ iterator, but it's okay
    // since the getNextWorker() implementation is robust.
    CHECK(1 == getHostWorkerPool(old_hostname).erase(shard))
      << "Inconsistency: did not find shard " << shard
      << " in the worker pool for its hostname " << old_hostname;
    CHECK(getHostWorkerPool(new_hostname).emplace(
      shard, worker_it->second
    ).second)
      << "Changing hostname " << old_hostname << " to " << new_hostname
      << ": target already had shard " << shard;
  }
  // Update the worker's state (including the hostname)
  return worker_it->second->processHeartbeat(update, worker);
  // TODO: Maybe remove everything in update->suicideWorkers_ from the
  // worker pools?
}

void RemoteWorkers::updateState(RemoteWorkerUpdate* update) {
  // Important to check this, but it's silly to check it inside the loop.
  CHECK(FLAGS_healthcheck_period > 0)
        << "--healthcheck_period must be positive";
  for (auto& pair : workerPool_) {
    pair.second->updateState(update);
  }
  // TODO: Maybe remove everything in update->suicideWorkers_ from the
  // worker pools?
}

const RemoteWorker*
RemoteWorkers::RoundRobinWorkerPool::getNextWorker() const {

  if (this->empty()) {
    LOG(WARNING) << "No workers in the '" << name_ << "' pool";
    return nullptr;
  }
  auto ret = this->find(nextShard_);
  if (ret == this->end()) {
    ret = this->begin();
  }
  auto* worker = ret->second.get();
  nextShard_ = (++ret == this->end()) ? this->begin()->first : ret->first;
  return worker;
}

RemoteWorkers::RoundRobinWorkerPool&
    RemoteWorkers::getHostWorkerPool(const string& host) {

  auto host_worker_it = hostToWorkerPool_.find(host);
  if (host_worker_it == hostToWorkerPool_.end()) {
    host_worker_it = hostToWorkerPool_.emplace(
      host, RoundRobinWorkerPool(host)  // Construct only if needed
    ).first;
  }
  return host_worker_it->second;
}

}}
