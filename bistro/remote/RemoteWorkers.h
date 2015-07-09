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

#include <folly/Optional.h>
#include <string>
#include <unordered_map>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

class RemoteWorker;
class RemoteWorkerUpdate;
class TaskStatus;

/**
 * Forwards RemoteWorkerRunner requests to the appropriate
 * RemoteWorker(s).  Provides round-robin selection of workers,
 * either global, or by host.
 *
 * WARNING: Not thread-safe, the caller must provide its own mutex.
 *
 * TODO(reviewers): Right now, RemoteWorkerRunner locks RemoteWorkers.  It's
 * possible that some applications would benefit from locking each
 * individual RemoteWorker.  How can I measure contention on the
 * RemoteWorkers lock and make an intelligent decision about this?
 */
class RemoteWorkers {
private:
//XXX eliminate clowntown
  // Inheritance saves a lot of boilerplate over composition
  class RoundRobinWorkerPool : public std::unordered_map<
    // Map of shard => worker connection
    std::string, std::shared_ptr<RemoteWorker>
  > {
  public:
    explicit RoundRobinWorkerPool(const std::string name) : name_(name) {}

    /**
     * Robust iterator: if nextShard_ isn't in the pool, use a random element.
     * If the pool is empty, returns nullptr.
     */
    const RemoteWorker* getNextWorker() const;

  private:
    const std::string name_;  // for log messages
    mutable std::string nextShard_;
  };

public:
  RemoteWorkers() : workerPool_("all workers") {}

  RemoteWorkers(const RemoteWorkers&) = delete;
  RemoteWorkers(RemoteWorkers&&) = delete;
  RemoteWorkers& operator=(const RemoteWorkers&) = delete;
  RemoteWorkers& operator=(RemoteWorkers&&) = delete;

  RoundRobinWorkerPool::const_iterator begin() const {
    return workerPool_.begin();
  }

  RoundRobinWorkerPool::const_iterator end() const {
    return workerPool_.end();
  }

  folly::Optional<cpp2::SchedulerHeartbeatResponse> processHeartbeat(
    RemoteWorkerUpdate* update,
    const cpp2::BistroWorker& worker
  );

  void updateState(RemoteWorkerUpdate* update);

  RemoteWorker* mutableWorkerOrAbort(const std::string& shard) {
    auto w = getNonConstWorker(shard);
    CHECK(w != nullptr) << "Unknown RemoteWorker: " << shard;
    return w;
  }

  RemoteWorker* mutableWorkerOrThrow(const std::string& shard) {
    auto w = getNonConstWorker(shard);
    if (w == nullptr) {
      throw BistroException("Unknown RemoteWorker: ", shard);
    }
    return w;
  }

  // Return nullptr if there's no worker with this shard ID
  const RemoteWorker* getWorker(const std::string& shard) {
    return getNonConstWorker(shard);
  }

  // Returns nullptr if no worker is available
  const RemoteWorker* getNextWorker() const {
    return workerPool_.getNextWorker();
  }

  // Returns nullptr if no worker is available on that host
  const RemoteWorker* getNextWorkerByHost(
    const std::string &hostname
  ) { return getHostWorkerPool(hostname).getNextWorker(); }

private:
  RemoteWorker* getNonConstWorker(const std::string& shard) {
    auto it = workerPool_.find(shard);
    if (it == workerPool_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  /**
   * If hostname isn't found, makes an empty worker pool for more concise code.
   */
  RoundRobinWorkerPool& getHostWorkerPool(const std::string& hostname);

  RoundRobinWorkerPool workerPool_;
  // Per-host round-robin, with the pointers shared with workerPool_
  std::unordered_map<std::string, RoundRobinWorkerPool> hostToWorkerPool_;
};

}}
