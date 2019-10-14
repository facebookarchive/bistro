/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/serialization/strong_typedef.hpp>
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
    const folly::dynamic& job_config,
    JobFilters::NodeDoesPassCob filter_cb = nullptr
  );

  explicit Job(const Job& j) = default;
  // TODO: Figure out why 'noexcept' does not compile here
  Job(Job&&) = default;  /* may throw */
  Job& operator=(Job&&) = default;

  inline bool isValid() const { return errors_.empty(); }
  inline bool canRun() const { return enabled_ && isValid(); }
  inline ID id() const { return id_; }
  inline const std::string& name() const { return name_; }
  inline const ResourceVector& resources() const { return resources_; }
  inline const folly::dynamic& config() const { return config_; }
  inline int levelForTasks() const { return levelForTasks_; }
  inline double priority() const { return priority_; }
  inline const std::string& owner() const { return owner_; }
  inline const folly::dynamic& errors() const { return errors_; }
  inline const std::vector<JobFilters>& filters() const { return filters_; }
  inline int levelForHostPlacement() const { return levelForHostPlacement_; }
  inline const std::string& hostPlacement() const { return hostPlacement_; }
  inline const JobBackoffSettings& backoffSettings() const {
    return backoffSettings_;
  }
  inline const std::vector<ID>& dependencies() const { return dependsOn_; }
  inline time_t createTime() const { return createTime_; }
  inline time_t modifyTime() const { return modifyTime_; }
  inline folly::Optional<std::chrono::milliseconds> killOrphanTasksAfter()
    const { return killOrphanTasksAfter_; }
  inline const cpp2::TaskSubprocessOptions& taskSubprocessOptions()
    const { return taskSubprocessOptions_; }
  inline const cpp2::KillRequest& killRequest() const { return killRequest_; }

  // ConfigLoaders may notice that a job config was modified and bump the time.
  void setModifyTime(time_t modify_time) { modifyTime_ = modify_time; }

  /**
   * Task placement constraint: For remote workers, this allows the job to
   * specify a mapping from a node to the worker host that must run this
   * task.
   */
  std::string requiredHostForTask(const Node& node) const;

  /**
   * For runners that execute shell commands, we allow the the job to either
   * use the default --worker_command (LocalRunner & RemoteWorkerRunner), or
   * to provide their own. This is executed when this is nonempty.
   */
  const std::vector<std::string>& command() const { return command_; }

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
  // IMPORTANT: ALL fields must have a default, preferably one set via a
  // default-initializer below, or in the constructor for non-POD values.
  // The default is used when the field fails to parse, or when it was not
  // set.  Parse failures are additionally recorded in errors_.

  ID id_;
  std::string name_;
  // enabled_ has no getter, since the right thing to test is canRun().  Why
  // not overwrite enabled_ with 'false' when some field is invalid?  That
  // would lose the user's initial setting, which would be frustrating.
  bool enabled_{true};
  std::string owner_;
  int levelForTasks_;
  double priority_{1.0};
  ResourceVector resources_;
  // The "config" JSON array which is passed through to all tasks (the
  // composition of Config::defaultJobConfig and the "config" key).
  folly::dynamic config_;
  std::vector<JobFilters> filters_;
  int levelForHostPlacement_{StringTable::NotFound};  // Default: no constraint
  std::string hostPlacement_;  // Empty string for no constraint
  JobBackoffSettings backoffSettings_;
  std::vector<ID> dependsOn_;
  // Positive only if the current ConfigLoader provides this field.
  time_t createTime_{0};
  time_t modifyTime_{0};
  folly::Optional<std::chrono::milliseconds> killOrphanTasksAfter_;
  cpp2::TaskSubprocessOptions taskSubprocessOptions_;
  cpp2::KillRequest killRequest_;
  std::vector<std::string> command_;

  // ConfigLoaders can use this to implement compare-and-swap mutation in
  // saveJob, thereby preventing races between concurrent saveJob() calls.
  int64_t versionID_{-1};

  // Error-handling is crucial for jobs, since a human user of Bistro may
  // alternate saveJob, loadJob, saveJob, etc, while making some mistakes.
  // To produce a good user experience, we will parse all the valid parts of
  // the configuration, while recording the un-parseable part in `errors_`
  // using the DynamicParser::RECORD format (see its docs).
  //
  // The subtle part is that `errors_` is exported as "errors", and we must
  // thus parse incoming "errors", since a load-save cycle should not lose
  // exiting error information.  We tell DynamicParser to record these in a
  // nested fashion by throwing a parse error whenever "errors" is set but
  // non-empty.  If that was the __only__ error in the config, we remove the
  // additional nesting so that the config doesn't grow needlessly.
  //
  // Future: It would be far better if saveJob()'s contract was such that it
  // would never accept jobs that parse with errors.  Do that in the future.
  // However, most of Bistro's config loaders are such that their content
  // can be altered indirectly (i.e. by editing the stored job JSON array),
  // so it is still advantageous to be able to parse jobs that have errors.
  folly::dynamic errors_;
};

typedef std::shared_ptr<const Job> JobPtr;

}}  // namespace facebook::bistro

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
