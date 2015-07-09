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
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <folly/Optional.h>

#include "bistro/bistro/config/JobBackoffSettings.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/RemoteWorkerSelectorType.h"
#include "bistro/bistro/config/SchedulerType.h"
#include "bistro/bistro/scheduler/ResourceVector.h"
#include "bistro/bistro/utils/SettingsMap.h"
#include "bistro/bistro/utils/SymbolTable.h"
#include <folly/dynamic.h>

namespace facebook { namespace bistro {

class Job;
typedef std::shared_ptr<const Job> JobPtr;

struct NodeConfig {
  std::string source;
  SettingsMap prefs;

  NodeConfig(const std::string& s, const folly::dynamic& p)
    : source(s), prefs(p) {}
};

class Config {
public:
  explicit Config(const folly::dynamic& settings);

  explicit Config(const Config&) = default;
  // The defaults used to be unsafe due to a design flaw in Job, but
  // now that it is fixed, it might be okay to re-introduce these.
  Config(Config&&) = delete;
  Config& operator=(Config&&) = delete;
  Config& operator=(const Config&) = delete;

  /**
   * Helper for ConfigLoaders. Use instead of mutating jobs directly.
   *
   * WARNING: Do NOT call from ConfigLoader::saveJob, or in any other way
   * that bypasses the ConfigLoader's storage (see 'jobs' comment below).
   */
  enum class JobStatus { NEW_CONFIG, ADDED, UPDATED, UNCHANGED };
  JobStatus addJob(
    const std::string& name,
    const folly::dynamic& d,
    // Tests may set this to nullptr, but ConfigLoaders should do their
    // darndest to set this correctly (this massively improves logging).
    const Config* prev_config,
    // A slightly ugly way of giving the caller non-const access to the Job
    Job** job_ptr = nullptr
  );

  // Only counts explicitly given levels, excluding "instance" and "worker".
  int getNumConfiguredLevels() const { return levels.size() - 2; }

  bool enabled;
  std::chrono::milliseconds idleWait;
  std::chrono::milliseconds workingWait;
  folly::Optional<std::chrono::milliseconds> killOrphanTasksAfter;
  std::vector<NodeConfig> nodeConfigs;
  StringTable levels;
  int levelForTasks;  // The default for jobs that don't specify one
  // WARNING: WARNING: WARNING: Do not manually mutate the job list (or any
  // part of the config) in response to e.g. "save job" or "delete job"
  // queries, or you will introduce configuration ping-ponging and
  // hard-to-find heisenbugs.  Any changes must propagate through the proper
  // ConfigLoader to ensure that configs have a monotonic history.
  std::unordered_map<std::string, JobPtr> jobs;
  SchedulerType schedulerType;
  RemoteWorkerSelectorType remoteWorkerSelectorType;
  ResourceVector defaultJobResources;
  // Weights are used e.g. to decide which worker is most loaded when
  // attempting to maximize the number of idle workers.
  ResourceVector resourceIDToWeight;
  std::vector<ResourceVector> resourcesByLevel;
  std::vector<std::vector<int>> levelIDToResourceID;
  StringTable resourceNames;
  JobBackoffSettings defaultBackoffSettings;

  // Optional override for worker resources, to indicate that some workers have
  // more resources available.
  std::unordered_map<std::string, ResourceVector> workerResourcesOverride;
};

namespace detail {
void parseKillOrphanTasksAfter(  // Used by Config & Job
  const folly::dynamic& d,
  folly::Optional<std::chrono::milliseconds>* maybe_kill_orphans
);
}

}}  // namespace facebook::bistro
