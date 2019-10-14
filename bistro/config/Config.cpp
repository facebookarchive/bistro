/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/Config.h"

#include <boost/algorithm/string/predicate.hpp>
#include <folly/Conv.h>
#include <folly/Memory.h>
#include <folly/json.h>
#include <folly/experimental/DynamicParser.h>
#include <limits>
#include <thread>

#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/parsing_common.h"
#include "bistro/bistro/scheduler/SchedulerPolicyRegistry.h"

// Future: remove this once there is nobody using this.
DEFINE_bool(
  kill_job_if_disabled, false,
  "Deprecated in favor of the runtime (JSON) Bistro setting of "
  "kill_orphan_tasks_after_sec. Setting this to 1 or true is "
  "equivalent to \"kill_orphan_tasks_after_sec\": 0, but the "
  "JSON setting takes precedence."
);

namespace facebook { namespace bistro {

namespace {
const ResourceVector& workerLevelResourcesRef(const Config& config) {
  const int worker_level_id = checkedLookup(config.levels, "worker");
  CHECK(worker_level_id != StringTable::NotFound);
  CHECK(worker_level_id > 0);
  CHECK(worker_level_id < config.resourcesByLevel.size());
  return config.resourcesByLevel[worker_level_id];
}

// You must also update RemoteWorkerRunner.cpp with the policy implementation.
const std::map<
  cpp2::PhysicalResource, std::set<cpp2::PhysicalResourceEnforcement>
> kResourceToSupportedEnforcements = {
  {cpp2::PhysicalResource::CPU_CORES, {
    cpp2::PhysicalResourceEnforcement::NONE,
    cpp2::PhysicalResourceEnforcement::SOFT,
  }},
  {cpp2::PhysicalResource::RAM_MBYTES, {
    cpp2::PhysicalResourceEnforcement::NONE,
    cpp2::PhysicalResourceEnforcement::HARD,
  }},
  {cpp2::PhysicalResource::GPU_CARDS, {
    cpp2::PhysicalResourceEnforcement::NONE,
  }},
};

cpp2::PhysicalResource physicalResourceFromString(const std::string& name) {
  if (name == kRamMB) {
    return cpp2::PhysicalResource::RAM_MBYTES;
  } else if (name == kCPUCore) {
    return cpp2::PhysicalResource::CPU_CORES;
  } else if (name == kGPUCard) {
    return cpp2::PhysicalResource::GPU_CARDS;
  } else {
    // NB: Not supporting GPU_MBYTES since we don't have a usable
    // story for it at the moment.
    throw std::invalid_argument("Unknown physical resource type");
  }
}

cpp2::PhysicalResourceEnforcement physicalResourceEnforcementFromString(
    const std::string& name) {
  if (name == kNone) {
    return cpp2::PhysicalResourceEnforcement::NONE;
  } else if (name == kSoft) {
    return cpp2::PhysicalResourceEnforcement::SOFT;
  } else if (name == kHard) {
    return cpp2::PhysicalResourceEnforcement::HARD;
  } else {
    throw std::invalid_argument("Unknown resource enforcement type");
  }
}

void parsePhysicalResourceConfigs(
    folly::DynamicParser* p,
    const Config& config,  // Resources must be parsed already.
    std::vector<cpp2::PhysicalResourceConfig>* rcfgs,
    std::unordered_map<std::string, size_t>* logical_to_physical) {

  const auto num_worker_resources = workerLevelResourcesRef(config).size();
  p->optional(kPhysicalResources, [&]() {
    p->objectItems([&](const std::string& name, const folly::dynamic&) {
      rcfgs->emplace_back();
      auto& rcfg = rcfgs->back();
      const auto idx_of_rcfg = rcfgs->size() - 1;

      rcfg.physical = physicalResourceFromString(name);
      p->required(kLogicalResource, [&](std::string&& logical) {
        rcfg.logical = std::move(logical);
        rcfg.logicalResourceID = config.resourceNames.lookup(rcfg.logical);
        if (rcfg.logicalResourceID == StringTable::NotFound
            || rcfg.logicalResourceID >= num_worker_resources) {
          throw BistroException(
            "Physical resource maps to unknown resource ", rcfg.logical
          );
        }
        // Make an inverted index by the string name of the logical
        // resource, since that's what workers report.  Do this at parse
        // time, so that errors are attached to the appropriate entry.
        if (!logical_to_physical->emplace(rcfg.logical, idx_of_rcfg).second) {
          throw BistroException(
            "Logical resource used in multiple physical_resources entries"
          );
        }
      });
      p->optional(kMultiplyLogicalBy, [&](double mul) {
        if (std::abs(mul) < 1e-12) {
          throw BistroException("Too close to 0 machine precision");
        }
        rcfg.multiplyLogicalBy = mul;
      });
      p->optional(kEnforcement, [&](const std::string& enforcement) {
        rcfg.enforcement = physicalResourceEnforcementFromString(enforcement);
        auto it = kResourceToSupportedEnforcements.find(rcfg.physical);
        if (it == kResourceToSupportedEnforcements.end()
            || !it->second.count(rcfg.enforcement)) {
          throw BistroException(
            "Resource ", rcfg.physical, " does not support enforcement type"
          );
        }
      });
      p->optional(kPhysicalReserveAmount, [&](double reserve) {
        // Allow negative values for now... maybe there is some use?
        rcfg.physicalReserveAmount = reserve;
      });
    });
  });
}
}  // anonymous namespace

// IMPORTANT: You MUST set each field's default **unconditionally** and
// BEFORE attempting to parse the field.  See the "IMPORTANT" note in
// Config.h for an explanation of Bistro's policy on defaults in the
// presence of parse errors.
Config::Config(const folly::dynamic& d_config)
  : idleWait(std::chrono::milliseconds(5000)),
    workingWait(std::chrono::milliseconds(500)),
    levels({"instance"}),
    schedulerPolicyName(kSchedulePolicyRoundRobin.str()) {

  folly::DynamicParser p(folly::DynamicParser::OnError::RECORD, &d_config);

  p.optional(kEnabled, [&](bool b) { enabled = b; });
  p.optional("working_wait", [&](double n) {
    workingWait = std::chrono::milliseconds(static_cast<int>(1000 * n));
  });
  p.optional("idle_wait", [&](double n) {
    idleWait = std::chrono::milliseconds(static_cast<int>(1000 * n));
  });
  p.optional("scheduler", [&](const std::string& s) {
    schedulerPolicyName = s;
    // Invalid value? Throw at parse-time rather than at runtime.
    throwUnlessPolicyNameExists(schedulerPolicyName);
  });
  p.optional("remote_worker_selector", [&](const std::string& s) {
    remoteWorkerSelectorType = getRemoteWorkerSelectorType(s);
  });
  p.optional(kBackoff, [&](const folly::dynamic& value) {
    defaultBackoffSettings = JobBackoffSettings(value);
  });
  p.required(kNodes, [&]() {
    p.optional(kNodeOrder, [&](const std::string& s) {
      nodeOrderType = getNodeOrderType(s);
    });
    p.required("levels", [&]() {
      p.arrayItems([&](std::string&& s) { levels.insert(s); });
    });
    p.optional("node_sources", [&]() {
      p.arrayItems([&]() {
        folly::dynamic d_prefs = folly::dynamic::object();
        p.optional("prefs", [&]() { d_prefs = p.value(); });
        p.required("source", [&](std::string&& s) {
          nodeConfigs.emplace_back(std::move(s), std::move(d_prefs));
        });
      });
    });
  });
  // No matter what may be wrong with the config, add a 'worker' level as
  // the bottom level.
  levels.insert("worker");
  if (nodeConfigs.empty()) {
    LOG(WARNING) << "Empty 'node_sources', can only run on the instance node.";
  }

  if (FLAGS_kill_job_if_disabled) {  // A deprecated command-line default
    killOrphanTasksAfter = std::chrono::milliseconds(0);
  }
  parseKillOrphanTasksAfter(&p, &killOrphanTasksAfter);

  parseTaskSubprocessOptions(&p, &taskSubprocessOptions);

  // This is outside of TaskSubprocessOptions, so that it can be changed
  // even while the task is running.
  parseKillRequest(&p, &killRequest);

  // This is a DANGEROUS setting, see the doc at the declaration site.
  p.optional(kExitInitialWaitBeforeTimestamp, [&](int64_t n) {
    exitInitialWaitBeforeTimestamp = n;
  });

  // What is the node level that jobs will use to create tasks by default?
  // You probably don't want to specify "worker" because there are no nodes.
  // It's not forbidden in the spirit of laissez-faire.
  //
  // Default to the bottom level ("instance" is 0, "worker" is last).
  levelForTasks = getNumConfiguredLevels();
  p.optional(kLevelForTasks, [&](const std::string& s) {
    levelForTasks = checkedLookup(levels, s);
  });

  resourcesByLevel.resize(levels.size());
  levelIDToResourceID.resize(levels.size());
  p.required("resources", [&]() {
    p.objectItems([&](const std::string& level, const folly::dynamic&) {
      int level_id = checkedLookup(levels, level);
      auto& level_rsrc = resourcesByLevel[level_id];
      p.objectItems([&](const std::string& name, const folly::dynamic&) {
        const int resource_id = resourceNames.insert(name);
        // This inverse index MUST happen as soon as we insert into
        // resourceNames, see the note on the "limit" default below.
        levelIDToResourceID[level_id].push_back(resource_id);

        // IMPORTANT: Nothing from this point onward throws, except inside
        // the parses, which are protected by DynamicParser (see "weight"
        // below for an acceptable example).  This is important so that the
        // various resource structures stay consistent with each other.

        defaultJobResources.resize(resource_id + 1, 0);
        p.required("default", [&](int64_t n) {
          defaultJobResources[resource_id] = n;
        });

        // This is pretty crazy: each level has slots for the resources with
        // IDs from 0 to the maximum resource ID for that level.  Most of
        // those IDs do not belong to this level, so those slots are just set
        // to INT_MAX, and we pray they do not run out.
        level_rsrc.resize(resource_id + 1, std::numeric_limits<int>::max());
        // This default is critical, for a subtle reason. In this parse, we
        // already committed to this being a valid resouce ID.  If "limit"
        // failed to parse and we left this at INT_MAX, then finding this ID
        // in levelIDToResourceID and using resourcesByLevel would yield a
        // sentinel value meaning "this level lacks this resource".  That's
        // a recipe for a CHECK failure.  It would be even worse not to
        // insert into levelIDToResourceID until "limit" is parsed, since
        // that would break Job::toDynamic() -- we wouldd cleanly parse such
        // a resource, and then fail to enumerate it via
        // levelIDToResourceID, resulting in data-loss on save/load cycles.
        // This default avoids both problems.
        level_rsrc[resource_id] = 0;
        p.required("limit", [&](int64_t n) {
          level_rsrc[resource_id] = n;
        });

        // Defaulting to a weight of 0 means that resource won't be a factor
        // when using the "busiest" worker selection strategy.
        resourceIDToWeight.resize(resource_id + 1, 0);
        p.optional("weight", [&](int64_t n) {
          if (n < 0) { throw BistroException("Must be >= 0"); }
          resourceIDToWeight[resource_id] = n;
        });
      });
    });
  });

  p.optional(kWorkerResourceOverride, [&]() {
    const auto& default_rsrcs = workerLevelResourcesRef(*this);
    p.objectItems([&](const std::string& worker, const folly::dynamic&) {
      auto& worker_overrides = workerResourcesOverride[worker];
      p.objectItems([&](const std::string& resource_name, int64_t amount) {
        const int resource_id = checkedLookup(resourceNames, resource_name);
        if (resource_id >= default_rsrcs.size()) {
          throw BistroException("Overriding unknown resource");
        }
        // If a resource is set to max, then we haven't set it for this level.
        if (default_rsrcs[resource_id] == std::numeric_limits<int>::max()) {
          throw BistroException("Overriding resource for wrong level");
        }
        worker_overrides.emplace_back(resource_id, amount);
      });
    });
  });

  parsePhysicalResourceConfigs(
    &p, *this, &physicalResourceConfigs, &logicalToPhysical
  );

  auto errors = p.releaseErrors();
  if (!errors.empty()) {
    throw BistroException("Invalid config: ", folly::toPrettyJson(errors));
  }
}

Config::JobStatus Config::addJob(
    std::shared_ptr<Job> job,
    const Config* prev_config) {
  auto p = jobs.emplace(job->name(), job);
  if (!p.second) {
    throw BistroException("Adding a job that already exists: ", job->name());
  }
  if (prev_config) {
    auto it = prev_config->jobs.find(job->name());
    if (it != prev_config->jobs.end()) {
      auto new_d = job->toDynamic(*this);
      // Slower than a dedicated comparator, but less likely to have bugs.
      if (new_d == it->second->toDynamic(*prev_config)) {
        return Config::JobStatus::UNCHANGED;
      }
      // This and the next log both seem useful for all config loaders.
      LOG(INFO) << "Job " << job->name() << " was modified: "
        << folly::toJson(new_d);
      return Config::JobStatus::UPDATED;
    }
    LOG(INFO) << "Job " << job->name() << " was added: "
      << folly::toJson(job->toDynamic(*this));
    return Config::JobStatus::ADDED;
  }
  return Config::JobStatus::NEW_CONFIG;
}

}}  // namespace facebook::bistro
