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
#include "bistro/bistro/config/NodeOrderType.h"
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

  bool enabled{false};
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
  SchedulerType schedulerType{SchedulerType::RoundRobin};
  RemoteWorkerSelectorType remoteWorkerSelectorType{
    RemoteWorkerSelectorType::RoundRobin
  };

  NodeOrderType nodeOrderType{NodeOrderType::Random};
  ResourceVector defaultJobResources;
  // Weights are used e.g. to decide which worker is most loaded when
  // attempting to maximize the number of idle workers.
  ResourceVector resourceIDToWeight;
  std::vector<ResourceVector> resourcesByLevel;
  std::vector<std::vector<int>> levelIDToResourceID;
  StringTable resourceNames;

  JobBackoffSettings defaultBackoffSettings;
  cpp2::TaskSubprocessOptions taskSubprocessOptions;
  cpp2::KillRequest killRequest;
  cpp2::PhysicalResourceConfigs physicalResourceConfigs;
  // Inverted index: contains pointers into physicalResourceConfigs.
  std::unordered_map<std::string, cpp2::PhysicalResourceConfig const*>
    logicalToPhysical;

  // A DANGEROUS manual toggle allowing the scheduler's administrator to
  // immediately exit initial wait.  It remains in effect only until the
  // specified timestamp passes.  The intended usage is: set the value for
  // 1-2 minutes in the future to exit initial wait immediately.  Set it to
  // a longer period ***only*** if you expect to be iterating on the
  // scheduler settings in a debugging setting.  DANGER: If tasks are
  // running on live workers, and you use this setting, you WILL
  // double-start tasks.
  int64_t exitInitialWaitBeforeTimestamp{0};  // 1970 is the same as "never"

  // Optional override for worker resources, to indicate that some workers have
  // more resources available.
  std::unordered_map<std::string, ResourceVector> workerResourcesOverride;
};

// Helper functions used by Config & Job
namespace detail {
void parseKillOrphanTasksAfter(
  const folly::dynamic& d,
  folly::Optional<std::chrono::milliseconds>* maybe_kill_orphans
);
folly::dynamic taskSubprocessOptionsToDynamic(
  const cpp2::TaskSubprocessOptions& opts
);
void parseTaskSubprocessOptions(
  const folly::dynamic& d,
  cpp2::TaskSubprocessOptions* opts
);
folly::dynamic killRequestToDynamic(const cpp2::KillRequest& req);
void parseKillRequest(const folly::dynamic& d, cpp2::KillRequest* req);
}  // namespace detail

// Field names: better duplicated string constants than typo-prone literals.
namespace {
// Top-level config keys
const char* kExitInitialWaitBeforeTimestamp =
  "CAUTION_exit_initial_wait_before_timestamp";  // DANGEROUS, see doc above.
// Nodes
const char* kNodeOrder = "node_order";
// Task subprocess
const char* kTaskSubprocess = "task_subprocess";
const char* kPollMs = "poll_ms";
const char* kMaxLogLinesPerPollInterval
  = "max_log_lines_per_poll_interval";
const char* kParentDeathSignal = "parent_death_signal";
const char* kProcessGroupLeader = "process_group_leader";
const char* kUseCanaryPipe = "use_canary_pipe";
// Kill request
const char* kKillSubprocess = "kill_subprocess";
const char* kMethod = "method";
const char* kTermWaitKill = "term_wait_kill";
const char* kTerm = "term";
const char* kKill = "kill";
const char* kKillWaitMs = "kill_wait_ms";
// CGroups
const char* kCGroups = "cgroups";
const char* kRoot = "root";
const char* kSlice = "slice";
const char* kSubsystems = "subsystems";
const char* kKillWithoutFreezer = "kill_without_freezer";
// Physical resource configuration
const char* kPhysicalResources = "physical_resources";
// The unit names are all singular, since each entry just maps a physical
// unit to a logical unit.
const char* kRamMB = "ram_mb";
const char* kCPUCore = "cpu_core";
const char* kGPUCard = "gpu_card";
// Keys of physical resource configs
const char* kLogicalResource = "logical_resource";
const char* kMultiplyLogicalBy = "multiply_logical_by";
const char* kEnforcement = "enforcement";
// Enforcement options
const char* kNone = "none";
const char* kSoft = "soft";
const char* kHard = "hard";
}  // anonymous namespace

}}  // namespace facebook::bistro
