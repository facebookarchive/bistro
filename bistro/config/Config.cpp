/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/config/Config.h"

#include <boost/algorithm/string/predicate.hpp>
#include <folly/Conv.h>
#include <folly/Memory.h>
#include <folly/json.h>
#include <limits>
#include <thread>

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/utils/Exception.h"

// Future: remove this once there is nobody using this.
DEFINE_bool(
  kill_job_if_disabled, false,
  "Deprecated in favor of the runtime (JSON) Bistro setting of "
  "kill_orphan_tasks_after_sec. Setting this to 1 or true is "
  "equivalent to \"kill_orphan_tasks_after_sec\": 0, but the "
  "JSON setting takes precedence."
);

namespace facebook { namespace bistro {

namespace {  // If useful, export these to some utility file.
const folly::dynamic* dynObjectPtr(
    const folly::dynamic& d,
    const folly::dynamic& key) {
  const auto* p = d.get_ptr(key);
  if (p && !p->isObject()) {
    throw BistroException(key.asString(), " must be an object");
  }
  return p;
}

const folly::dynamic* dynArrayPtr(
    const folly::dynamic& d,
    const folly::dynamic& key) {
  const auto* p = d.get_ptr(key);
  if (p && !p->isArray()) {
    throw BistroException(key.asString(), " must be an array");
  }
  return p;
}

const folly::Optional<std::string> dynGetString(
    const folly::dynamic& d,
    const folly::dynamic& key) {
  if (const auto* p = d.get_ptr(key)) {
    if (!p->isString()) {
      throw BistroException(key.asString(), " must be a string");
    }
    return p->asString().toStdString();
  }
  return folly::none;
}

const folly::Optional<bool> dynGetBool(
    const folly::dynamic& d,
    const folly::dynamic& key) {
  if (const auto* p = d.get_ptr(key)) {
    if (!p->isBool()) {
      throw BistroException(key.asString(), " must be a boolean");
    }
    return p->asBool();
  }
  return folly::none;
}

const folly::Optional<int64_t> dynGetInt(
    const folly::dynamic& d,
    const folly::dynamic& key) {
  if (const auto* p = d.get_ptr(key)) {
    if (!p->isInt()) {
      throw BistroException(key.asString(), " must be an integer");
    }
    return p->asInt();
  }
  return folly::none;
}

const folly::Optional<double> dynGetDouble(
    const folly::dynamic& d,
    const folly::dynamic& key) {
  if (const auto* p = d.get_ptr(key)) {
    if (!p->isDouble() && !p->isInt()) {
      throw BistroException(key.asString(), " must be a double");
    }
    return p->asDouble();
  }
  return folly::none;
}

const ResourceVector& workerLevelResourcesRef(const Config& config) {
  const int worker_level_id = config.levels.lookup("worker");
  CHECK(worker_level_id != StringTable::NotFound);
  CHECK(worker_level_id > 0);
  CHECK(worker_level_id < config.resourcesByLevel.size());
  return config.resourcesByLevel[worker_level_id];
}
}  // anonymous namespace

namespace detail {
void parseKillOrphanTasksAfter(
    const folly::dynamic& d,
    folly::Optional<std::chrono::milliseconds>* maybe_kill_orphans) {
  if (const auto* kill_orphans = d.get_ptr("kill_orphan_tasks_after_sec")) {
    if (kill_orphans->isBool()) {
      if (kill_orphans->asBool()) {
        *maybe_kill_orphans = std::chrono::milliseconds(0);
      } else {
        maybe_kill_orphans->clear();  // false overrides the command-line arg
      }
    } else if (kill_orphans->isNumber()) {
      auto v = kill_orphans->asDouble();
      if (v >= 0.0) {
        *maybe_kill_orphans =
          std::chrono::milliseconds(static_cast<int>(1000 * v));
      }
    } else {
      throw BistroException(
        "kill_orphan_tasks_after_sec must be a number or a boolean"
      );
    }
  }
}

folly::dynamic taskSubprocessOptionsToDynamic(
    const cpp2::TaskSubprocessOptions& opts) {
  return folly::dynamic::object
    (kPollMs, opts.pollMs)
    (kMaxLogLinesPerPollInterval, opts.maxLogLinesPerPollInterval)
    (kParentDeathSignal, opts.parentDeathSignal)
    (kProcessGroupLeader, opts.processGroupLeader)
    (kUseCanaryPipe, opts.useCanaryPipe);
}

void parseTaskSubprocessOptions(
    const folly::dynamic& d,
    cpp2::TaskSubprocessOptions* opts) {
  if (const auto* tso = dynObjectPtr(d, kTaskSubprocess)) {
    if (const auto p = dynGetInt(*tso, kPollMs)) {
      opts->pollMs = *p;
    }
    if (const auto p = dynGetInt(*tso, kMaxLogLinesPerPollInterval)) {
      opts->maxLogLinesPerPollInterval = *p;
    }
    if (const auto p = dynGetInt(*tso, kParentDeathSignal)) {
      opts->parentDeathSignal = *p;
    }
    if (const auto p = dynGetBool(*tso, kProcessGroupLeader)) {
      opts->processGroupLeader = *p;
    }
    if (const auto p = dynGetBool(*tso, kUseCanaryPipe)) {
      opts->useCanaryPipe = *p;
    }
    if (const auto* cgp = dynObjectPtr(*tso, kCGroups)) {
      auto& cgopts = opts->cgroupOptions;
      if (const auto p = dynGetString(*cgp, kRoot)) {
        cgopts.root = p->toStdString();
      }
      if (const auto p = dynGetString(*cgp, kSlice)) {
        cgopts.slice = p->toStdString();
      }
      if (const auto* p = dynArrayPtr(*cgp, kSubsystems)) {
        for (const auto& s : *p) {
          if (!s.isString()) {
            throw BistroException("cgroups subsystems entries must be strings");
          }
          cgopts.subsystems.emplace_back(s.asString().toStdString());
        }
      }
      if (const auto p = dynGetBool(*cgp, kKillWithoutFreezer)) {
        cgopts.killWithoutFreezer = *p;
      }
      // cpuShares and and memoryLimitInBytes will be populated on a
      // per-task basis, based on their worker resources using
      // PhysicalResourceConfig (below).  unitTestCreateFiles is for tests.
    }
  }
}

// You must also update RemoteWorkerRunner.cpp with the implementation.
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

void parsePhysicalResourceConfigs(
    const Config& config,  // Resources must be parsed already.
    const folly::dynamic& d_parent,
    std::vector<cpp2::PhysicalResourceConfig>* rcfgs) {
  if (const auto* prs = dynObjectPtr(d_parent, kPhysicalResources)) {
    for (const auto& name_and_d : prs->items()) {
      rcfgs->emplace_back();
      auto& rcfg = rcfgs->back();
      rcfg.physical = [](const folly::dynamic& name) {
        if (name == kRamMB) {
          return cpp2::PhysicalResource::RAM_MBYTES;
        } else if (name == kCPUCore) {
          return cpp2::PhysicalResource::CPU_CORES;
        } else if (name == kGPUCard) {
          return cpp2::PhysicalResource::GPU_CARDS;
        } else {
          // NB: Not supporting GPU_MBYTES since we don't have a usable
          // story for it at the moment.
          throw BistroException("Bad physical resource: ", name.asString());
        }
      }(name_and_d.first);
      if (!name_and_d.second.isObject()) {
        throw BistroException("physical_resources entries must be objects");
      }
      if (const auto p = dynGetString(name_and_d.second, kLogicalResource)) {
        rcfg.logical = p->toStdString();
        rcfg.logicalResourceID = config.resourceNames.lookup(rcfg.logical);
        auto num_worker_resources = workerLevelResourcesRef(config).size();
        if (rcfg.logicalResourceID == StringTable::NotFound
            || rcfg.logicalResourceID >= num_worker_resources) {
          throw BistroException(
            "Physical resource maps to unknown resource ", rcfg.logical
          );
        }
      } else {
        throw BistroException(
          kLogicalResource, " is required in physical_resources entries"
        );
      }
      if (const auto p = dynGetDouble(name_and_d.second, kMultiplyLogicalBy)) {
        if (*p < 1e-12) {  // Stay well away from machine precision
          throw BistroException(kMultiplyLogicalBy, " too small: ", *p);
        }
        rcfg.multiplyLogicalBy = *p;
      }
      if (const auto p = dynGetString(name_and_d.second, kEnforcement)) {
        if (*p == kNone) {
          rcfg.enforcement = cpp2::PhysicalResourceEnforcement::NONE;
        } else if (*p == kSoft) {
          rcfg.enforcement = cpp2::PhysicalResourceEnforcement::SOFT;
        } else if (*p == kHard) {
          rcfg.enforcement = cpp2::PhysicalResourceEnforcement::HARD;
        } else {
          throw BistroException("Bad resource enforcement type: ", *p);
        }
        auto it = kResourceToSupportedEnforcements.find(rcfg.physical);
        if (it == kResourceToSupportedEnforcements.end()
            || !it->second.count(rcfg.enforcement)) {
          throw BistroException(
            "Resource ", name_and_d.first.asString(), " does not support ",
            "enforcement type ", *p
          );
        }
      }
      if (auto p = dynGetDouble(name_and_d.second, kPhysicalReserveAmount)) {
        // Allow negative values for now... maybe there is some use?
        rcfg.physicalReserveAmount = *p;
      }
    }
  }
}

folly::dynamic killRequestToDynamic(const cpp2::KillRequest& req) {
  return folly::dynamic::object
    (kMethod, [&](){
        switch (req.method) {
          case cpp2::KillMethod::TERM_WAIT_KILL:
            return kTermWaitKill;
          case cpp2::KillMethod::TERM:
            return kTerm;
          case cpp2::KillMethod::KILL:
            return kKill;
          default:
            throw BistroException(
              "Unknown KillMethod ", static_cast<int>(req.method)
            );
        }
      }())
    (kKillWaitMs, req.killWaitMs);
}

void parseKillRequest(const folly::dynamic& d, cpp2::KillRequest* req) {
  if (const auto* kr = dynObjectPtr(d, kKillSubprocess)) {
    if (const auto p = dynGetString(*kr, kMethod)) {
      if (*p == kTermWaitKill) {
        req->method = cpp2::KillMethod::TERM_WAIT_KILL;
      } else if (*p == kTerm) {
        req->method = cpp2::KillMethod::TERM;
      } else if (*p == kKill) {
        req->method = cpp2::KillMethod::KILL;
      } else {
        throw BistroException("Unknown KillMethod ", *p);
      }
    }
    if (const auto p = dynGetInt(*kr, kKillWaitMs)) {
      req->killWaitMs = *p;
    }
  }
}
}  // namespace detail

using namespace folly;
using namespace std;

Config::Config(const dynamic& d)
  : idleWait(chrono::milliseconds(5000)),
    workingWait(chrono::milliseconds(500)),
    levels({"instance"}) {

  auto it = d.find("enabled");
  if (it != d.items().end()) {
    enabled = it->second.asBool();
  }

  it = d.find("working_wait");
  if (it != d.items().end()) {
    workingWait = chrono::milliseconds(static_cast<int>(
      1000 * it->second.asDouble()
    ));
  }

  it = d.find("idle_wait");
  if (it != d.items().end()) {
    idleWait = chrono::milliseconds(static_cast<int>(
      1000 * it->second.asDouble()
    ));
  }

  it = d.find("scheduler");
  if (it != d.items().end()) {
    schedulerType = getSchedulerType(it->second.asString());
  }

  if (const auto maybe_sel = dynGetString(d, "remote_worker_selector")) {
    remoteWorkerSelectorType = getRemoteWorkerSelectorType(*maybe_sel);
  }

  it = d.find("backoff");
  if (it != d.items().end()) {
    defaultBackoffSettings = JobBackoffSettings(it->second);
  }

  it = d.find("nodes");
  if (it == d.items().end() || !it->second.isObject()) {
    throw BistroException("Nodes is required in the config");
  }
  auto& node_settings = it->second;
  if (auto maybe_order = dynGetString(node_settings, kNodeOrder)) {
    nodeOrderType = getNodeOrderType(*maybe_order);
  }
  auto jt = node_settings.find("levels");
  if (jt == node_settings.items().end() || !jt->second.isArray()) {
    throw BistroException("Levels is required in the nodes config");
  }
  for (const dynamic& level : jt->second) {
    levels.insert(level.asString().toStdString());
  }
  // Add a 'worker' level as the bottom level (for resources only -- no nodes)
  levels.insert("worker");

  if (FLAGS_kill_job_if_disabled) {
    killOrphanTasksAfter = std::chrono::milliseconds(0);
  }
  detail::parseKillOrphanTasksAfter(d, &killOrphanTasksAfter);

  detail::parseTaskSubprocessOptions(d, &taskSubprocessOptions);

  detail::parseKillRequest(d, &killRequest);

  // This is a DANGEROUS setting, see the doc at the declaration site.
  if (const auto p = dynGetInt(d, kExitInitialWaitBeforeTimestamp)) {
    exitInitialWaitBeforeTimestamp = *p;
  }

  // What is the node level that jobs will use to create tasks by default?
  // You probably don't want to specify "worker" because there are no nodes.
  // It's not forbidden in the spirit of laissez-faire.
  if (const auto maybe_level_for_tasks = dynGetString(d, "level_for_tasks")) {
    levelForTasks = levels.lookup(*maybe_level_for_tasks);
    if (levelForTasks == StringTable::NotFound) {
      throw BistroException("Bad level_for_tasks: ", *maybe_level_for_tasks);
    }
  } else {  // Default to the bottom level (the one just before "worker")
    levelForTasks = levels.size() - 2;
  }

  // TODO(agoder): Deprecate this
  jt = node_settings.find("node_source");
  if (jt != node_settings.items().end()) {
    nodeConfigs.emplace_back(
      jt->second.asString().toStdString(),
      node_settings.getDefault("node_source_prefs")
    );
  }

  jt = node_settings.find("node_sources");
  if (jt != node_settings.items().end()) {
    for (const auto& setting : jt->second) {
      nodeConfigs.emplace_back(
        setting["source"].asString().toStdString(),
        setting.getDefault("prefs", dynamic::object)
      );
    }
  }

  if (nodeConfigs.empty()) {
    LOG(WARNING) << "Empty 'node_sources', can only run on the instance node.";
  }

  it = d.find("resources");
  if (it == d.items().end()) {
    throw BistroException("Must provide resources setting in config");
  }
  if (!it->second.isObject()) {
    throw BistroException("Resources setting must be an object");
  }
  resourcesByLevel.resize(levels.size());
  levelIDToResourceID.resize(levels.size());
  int resource_id = 0;
  for (const auto& by_level : it->second.items()) {
    const auto& level = by_level.first.asString().toStdString();
    if (!by_level.second.isObject()) {
      throw BistroException(
        "Resources for level ", level, " must be an object"
      );
    }
    for (const auto& pair : by_level.second.items()) {
      int level_id = levels.lookup(level);
      if (level_id == StringTable::NotFound) {
        throw BistroException("Invalid level: ", level);
      }
      const auto& name = pair.first.asString().toStdString();
      const int resource_id = resourceNames.insert(name);
      defaultJobResources.resize(resource_id + 1, 0);
      defaultJobResources[resource_id] = pair.second["default"].asInt();
      // Defaulting to a weight of 0 means that resource won't be a factor
      // when using the "busiest" worker selection strategy.
      resourceIDToWeight.resize(resource_id + 1, 0);
      if (auto* weight_ptr = pair.second.get_ptr("weight")) {
        if (!weight_ptr->isInt() || weight_ptr->asInt() < 0) {
          throw BistroException("Resource weight must be an integer >= 0");
        }
        resourceIDToWeight[resource_id] = weight_ptr->asInt();
      }
      auto& r = resourcesByLevel[level_id];
      // This is pretty crazy: each level has slots for the resources with
      // IDs from 0 to the maximum resource ID for that level.  Most of
      // those IDs do not belong to this level, so those slots are just set
      // to INT_MAX, and we pray they do not run out.
      r.resize(resource_id + 1, numeric_limits<int>::max());
      r[resource_id] = pair.second["limit"].asInt();
      levelIDToResourceID[level_id].push_back(resource_id);
    }
  }

  if (const auto* ptr = dynObjectPtr(d, "worker_resources_override")) {
    const auto& default_resources = workerLevelResourcesRef(*this);
    for (const auto& pair : ptr->items()) {
      const string& worker = pair.first.asString().toStdString();
      auto& worker_overrides = workerResourcesOverride[worker];
      for (const auto& item : pair.second.items()) {
        const string& resource_name = item.first.asString().toStdString();
        const int resource_id = resourceNames.lookup(resource_name);
        if ((resource_id == StringTable::NotFound)
            || (resource_id >= default_resources.size())) {
          throw BistroException("Overriding unknown resource ", resource_name);
        }
        // If a resource is set to max, then we haven't set it for this level.
        if (default_resources[resource_id] == numeric_limits<int>::max()) {
          throw BistroException(
            "Override resource for wrong level ",
            resource_name
          );
        }
        worker_overrides.emplace_back(std::make_pair(
          resource_id, item.second.asInt()
        ));
      }
    }
  }

  detail::parsePhysicalResourceConfigs(*this, d, &physicalResourceConfigs);
  // Make an inverted index by the string name of the logical resource,
  // since that's what workers report.
  for (size_t i = 0; i < physicalResourceConfigs.size(); ++i) {
    const auto& logical_name = physicalResourceConfigs[i].logical;
    if (!logicalToPhysical.emplace(logical_name, i).second) {
      throw BistroException(
        "logical resource ", logical_name, " used in more than one ",
        "physical_resources entry"
      );
    }
  }
}

Config::JobStatus Config::addJob(
    const std::string& name,
    const folly::dynamic& d,
    const Config* prev_config,
    Job** job_ptr) {
  auto job = std::make_shared<Job>(*this, name, d);
  auto j = job.get();  // This gets us a non-const pointer :)
  auto p = jobs.emplace(name, std::move(job));
  if (!p.second) {
    throw BistroException("Adding a job that already exists: ", name);
  }
  if (job_ptr) {
    *job_ptr = j;
  }
  if (prev_config) {
    auto it = prev_config->jobs.find(j->name());
    if (it != prev_config->jobs.end()) {
      auto new_d = j->toDynamic(*this);
      // Slower than a dedicated comparator, but less likely to have bugs.
      if (new_d == it->second->toDynamic(*prev_config)) {
        return Config::JobStatus::UNCHANGED;
      }
      // This and the next log both seem useful for all config loaders.
      LOG(INFO) << "Job " << j->name() << " was modified: "
        << folly::toJson(new_d);
      return Config::JobStatus::UPDATED;
    }
    LOG(INFO) << "Job " << j->name() << " was added: "
      << folly::toJson(j->toDynamic(*this));
    return Config::JobStatus::ADDED;
  }
  return Config::JobStatus::NEW_CONFIG;
}

}}
