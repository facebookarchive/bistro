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

#include <boost/strong_typedef.hpp>
#include <folly/Optional.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "bistro/bistro/config/JobFilters.h"
#include "bistro/bistro/utils/SymbolTable.h"
#include "bistro/bistro/config/JobBackoffSettings.h"
#include "bistro/bistro/scheduler/ResourceVector.h"
#include <folly/Synchronized.h>
#include <folly/dynamic.h>

namespace facebook { namespace bistro {

class Config;
class Node;

class Job {
public:
  BOOST_STRONG_TYPEDEF(int, ID);

  Job(
    const Config& parent_config,  // Copies some fields from the Config
    const std::string& job_name,
    const folly::dynamic& job_config
  );

  explicit Job(const Job& j) = default;
  // TODO: Figure out why 'noexcept' does not compile here
  Job(Job&&) = default;  /* may throw */
  Job& operator=(Job&&) = default;

  inline bool isValid() const { return error_.empty(); }
  inline bool isEnabled() const { return enabled_; }
  inline ID id() const { return id_; }
  inline const std::string& name() const { return name_; }
  inline const ResourceVector& resources() const { return resources_; }
  inline const folly::dynamic& config() const { return config_; }
  inline int levelForTasks() const { return levelForTasks_; }
  inline double priority() const { return priority_; }
  inline const std::string& owner() const { return owner_; }
  inline const std::string& error() const { return error_; }
  inline const std::vector<JobFilters>& filters() const { return filters_; }
  inline int levelForHostPlacement() const {
    return levelForHostPlacement_;
  }
  inline const std::string& hostPlacement() const { return hostPlacement_; }
  inline const JobBackoffSettings& backoffSettings() const {
    return backoffSettings_;
  }
  inline const std::vector<ID>& dependencies() const {
    return dependsOn_;
  }
  inline time_t createTime() const { return createTime_; }
  inline time_t modifyTime() const { return modifyTime_; }
  inline folly::Optional<std::chrono::milliseconds> killOrphanTasksAfter()
    const { return killOrphanTasksAfter_; }

  // ConfigLoaders may notice that a job config was modified and bump the time.
  void setModifyTime(time_t modify_time) { modifyTime_ = modify_time; }

  /**
   * Task placement constraint: For remote workers, this allows the job to
   * specify a mapping from a node to the worker host that must run this
   * task.
   */
  std::string requiredHostForTask(const Node& node) const;

  // shouldRunOn() returns this reason for whether this job can run on a given
  // node or not.
  enum ShouldRun {
    Yes,
    NoDisabled,
    NoAvoided,
  };

  ShouldRun shouldRunOn(const Node& node) const;

  folly::dynamic toDynamic(const Config& parent_config) const;

  static folly::Synchronized<StringTable> JobNameTable;

private:
  ID id_;
  std::string name_;
  bool enabled_;
  std::string owner_;
  int levelForTasks_;
  double priority_;
  ResourceVector resources_;
  // The "config" JSON array which is passed through to all tasks (the
  // composition of Config::defaultJobConfig and the "config" key).
  folly::dynamic config_;
  std::vector<JobFilters> filters_;
  int levelForHostPlacement_;  // StringTable::NotFound for no constraints
  std::string hostPlacement_;  // Empty string for no constraints
  std::string error_;
  JobBackoffSettings backoffSettings_;
  std::vector<ID> dependsOn_;
  // Positive only if the current ConfigLoader provides this field.
  time_t createTime_{0};
  time_t modifyTime_{0};
  folly::Optional<std::chrono::milliseconds> killOrphanTasksAfter_;

  // ConfigLoaders can use this to implement compare-and-swap mutation in
  // saveJob, thereby preventing two racing saveJob() calls from silently
  // clobbering one another.
  int64_t versionID_{-1};
};

typedef std::shared_ptr<const Job> JobPtr;

}}

namespace std {
  template<>
  struct hash<facebook::bistro::Job::ID>
  {
    size_t operator()(facebook::bistro::Job::ID id) const
    {
      return std::hash<int>()(static_cast<int>(id));
    }
  };
}
