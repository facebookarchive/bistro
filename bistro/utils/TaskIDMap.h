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
#include <memory>
#include <unordered_map>

namespace facebook { namespace bistro {

/**
 * DEPRECATED: If you want to use or extend this, you're moving in the wrong
 * direction, talk to lesha@.
 *
 * Stores one item per Bistro task, quickly retrieves a single item for a
 * job and node, or all items for a job.
 *
 * Useful for storing / retrieving a small number of items, e.g. those of
 * running tasks.  Wastes RAM with many items, wastes CPU with many lookups.
 */
template<typename T>
class TaskIDMap {
public:
  bool insert(const std::string& job, const std::string& node, const T& item) {
    return emplace(job, node, T(item));
  }

  bool emplace(const std::string& job, const std::string& node, T&& item) {
    const auto& it_job = items_.find(job);
    if (it_job == items_.end()) {
      return items_.emplace(
        job, std::unordered_map<std::string, T>{{node, std::move(item)}}
      ).second;
    }
    return it_job->second.emplace(node, std::move(item)).second;
  }

  // Get a single item, or folly::none if the job or job-node pair is unknown.
  const folly::Optional<const T> get(
      const std::string& job, const std::string& node) const {
    const auto& it_job = items_.find(job);
    if (it_job == items_.end()) {
      return folly::none;
    }
    const auto& it_item = it_job->second.find(node);
    if (it_item == it_job->second.end()) {
      return folly::none;
    }
    return it_item->second;
  }

  // Get all items for a job name, return an empty map if the job is unknown.
  std::unordered_map<std::string, T> get(const std::string& job) const {
    const auto& it_job = items_.find(job);
    if (it_job == items_.end()) {
      return std::unordered_map<std::string, T>();
    };
    return it_job->second;
  }

  size_t erase(const std::string& job, const std::string& node) {
    const auto& it_job = items_.find(job);
    if (it_job == items_.end()) {
      return 0;
    }
    return it_job->second.erase(node);
  }

private:
  std::unordered_map<std::string, std::unordered_map<std::string, T>> items_;
};

}}  // namespace facebook::bistro
