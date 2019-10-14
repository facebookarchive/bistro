/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
#include "bistro/bistro/scheduler/ResourceVector.h"
#include "bistro/bistro/utils/SettingsMap.h"
#include "bistro/bistro/utils/SymbolTable.h"
#include <folly/dynamic.h>

namespace facebook { namespace bistro {

class Job;
typedef std::shared_ptr<const Job> JobPtr;

struct NodeConfig {
  std::string source;
  // Future: Eliminate this in favor of just using DynamicParser everywhere?
  SettingsMap prefs;

  NodeConfig(std::string s, folly::dynamic p)
    : source(std::move(s)), prefs(std::move(p)) {}
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
    std::shared_ptr<Job> job,
    // Tests may set this to nullptr, but ConfigLoaders should do their
    // darndest to set this correctly (this massively improves logging).
    const Config* prev_config);

  // Only counts explicitly given levels, excluding "instance" and "worker".
  int getNumConfiguredLevels() const {
    CHECK_GE(levels.size(), 2);
    return levels.size() - 2;
  }

  // IMPORTANT: Default values are used BOTH when the value is omitted, and
  // when there is an error parsing the value.  The latter is important when
  // a user queries Bistro's config while it has a parsing error -- thanks
  // to DynamicParser, the result will contain both "errors" and the default
  // value for the field.  This enables the user to revert to the default by
  // simply erasing the "errors" key -- NO, you should not do that
  // automatically.  Review configuration changes with care.

  bool enabled{false};
  std::chrono::milliseconds idleWait;
  std::chrono::milliseconds workingWait;
  // none => do not kill; otherwise, kill once orphaned for this duration.
  folly::Optional<std::chrono::milliseconds> killOrphanTasksAfter;
  std::vector<NodeConfig> nodeConfigs;
  StringTable levels;
  // -1 isn't the default, the constructor defaults this to the bottom level.
  int levelForTasks{-1};
  // WARNING: WARNING: WARNING: Do not manually mutate the job list (or any
  // part of the config) in response to e.g. "save job" or "delete job"
  // queries, or you will introduce configuration ping-ponging and
  // hard-to-find heisenbugs.  Any changes must propagate through the proper
  // ConfigLoader to ensure that configs have a monotonic history.
  std::unordered_map<std::string, JobPtr> jobs;

  std::string schedulerPolicyName;

  RemoteWorkerSelectorType remoteWorkerSelectorType{
    RemoteWorkerSelectorType::RoundRobin
  };
  NodeOrderType nodeOrderType{NodeOrderType::Random};

  // All of the following resource-related structures **should** always
  // parse to an internally consistent state, even in the face of
  // configuration errors.  See the notes in Config.cpp.  However, relying
  // on this is discouraged.
  ResourceVector defaultJobResources;
  // Weights are used e.g. to decide which worker is most loaded when
  // attempting to maximize the number of idle workers.
  ResourceVector resourceIDToWeight;
  std::vector<ResourceVector> resourcesByLevel;
  std::vector<std::vector<int>> levelIDToResourceID;
  StringTable resourceNames;

  // All of these have sane default constructors
  JobBackoffSettings defaultBackoffSettings;
  cpp2::TaskSubprocessOptions taskSubprocessOptions;
  cpp2::KillRequest killRequest;

  std::vector<cpp2::PhysicalResourceConfig> physicalResourceConfigs;
  // Inverted index: contains indexes into physicalResourceConfigs.
  std::unordered_map<std::string, size_t> logicalToPhysical;

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
  std::unordered_map<
    // shard => resource IDs & amounts
    std::string, std::vector<std::pair<int, int>>
  > workerResourcesOverride;
};

// Top-level config keys
constexpr folly::StringPiece kExitInitialWaitBeforeTimestamp =
  "CAUTION_exit_initial_wait_before_timestamp";  // DANGEROUS, see doc above.
constexpr folly::StringPiece kWorkerResourceOverride =
  "worker_resources_override";
// Nodes
constexpr folly::StringPiece kNodeOrder = "node_order";
// Physical resource configuration
constexpr folly::StringPiece kPhysicalResources = "physical_resources";
// The unit names are all singular, since each entry just maps a physical
// unit to a logical unit.
constexpr folly::StringPiece kRamMB = "ram_mb";
constexpr folly::StringPiece kCPUCore = "cpu_core";
constexpr folly::StringPiece kGPUCard = "gpu_card";
// Keys of physical resource configs
constexpr folly::StringPiece kLogicalResource = "logical_resource";
constexpr folly::StringPiece kMultiplyLogicalBy = "multiply_logical_by";
constexpr folly::StringPiece kPhysicalReserveAmount =
  "physical_reserve_amount";
constexpr folly::StringPiece kEnforcement = "enforcement";
// Enforcement options
constexpr folly::StringPiece kNone = "none";
constexpr folly::StringPiece kSoft = "soft";
constexpr folly::StringPiece kHard = "hard";
// Job options
constexpr folly::StringPiece kCommand = "command";

constexpr folly::StringPiece kSchedulePolicyUnitTest = "unittest";
constexpr folly::StringPiece kSchedulePolicyRoundRobin = "roundrobin";
constexpr folly::StringPiece kSchedulePolicyRankedPriority = "ranked_priority";
constexpr folly::StringPiece kSchedulePolicyRandomPriority =
    "randomized_priority";
constexpr folly::StringPiece kSchedulePolicyLongTail = "long_tail";

}}  // namespace facebook::bistro
