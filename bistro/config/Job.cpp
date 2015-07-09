/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/config/Job.h"

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/utils.h"
#include "bistro/bistro/config/Node.h"
#include <folly/json.h>

namespace facebook { namespace bistro {

using namespace std;
using namespace folly;

Synchronized<StringTable> Job::JobNameTable = Synchronized<StringTable>();

Job::Job(const Config& config, const string& name, const dynamic& d)
  : id_(Job::JobNameTable->insert(name)),
    name_(name),
    enabled_(d.getDefault("enabled", true).asBool()),
    owner_(d.getDefault("owner", "").asString().toStdString()),
    levelForTasks_(config.levelForTasks),
    priority_(d.getDefault("priority", 1.0).asDouble()),
    resources_(config.defaultJobResources),
    config_(dynamic::object),
    filters_(config.levels.size()),
    levelForHostPlacement_(StringTable::NotFound),
    backoffSettings_(config.defaultBackoffSettings),
    killOrphanTasksAfter_(config.killOrphanTasksAfter) {

  try {
    if (owner_.empty()) {
      throw BistroException("Job ", name, " missing owner.");
    }

    if (const auto* level_for_tasks_ptr = d.get_ptr("level_for_tasks")) {
      if (!level_for_tasks_ptr->isString()) {
        throw BistroException("'level_for_tasks' must be a string for ", name);
      }
      const auto& str_level_for_tasks =
        level_for_tasks_ptr->asString().toStdString();
      int level_for_tasks = config.levels.lookup(str_level_for_tasks);
      if (level_for_tasks == StringTable::NotFound) {
        throw BistroException("Bad level_for_tasks: ", str_level_for_tasks);
      }
      levelForTasks_ = level_for_tasks;
    }

    auto it = d.find("resources");
    if (it != d.items().end()) {
      if (!it->second.isObject()) {
        throw BistroException("'resources' must be an object for ", name);
      }
      for (const auto& pair : it->second.items()) {
        const auto& name = pair.first.asString().toStdString();
        const int resource_id = config.resourceNames.lookup(name);
        if (resource_id == StringTable::NotFound) {
          throw BistroException("Invalid resource: ", name);
        }
        resources_[resource_id] = pair.second.asInt();
      }
    }

    it = d.find("config");
    if (it != d.items().end()) {
      if (!it->second.isObject()) {
        throw BistroException("'config' must be an object for ", name);
      }
      update(config_, it->second);
    }

    it = d.find("backoff");
    if (it != d.items().end()) {
      if (!it->second.isArray()) {
        throw BistroException("'backoff' must be an array for ", name);
      }
      backoffSettings_ = JobBackoffSettings(it->second);
    }

    it = d.find("filters");
    if (it != d.items().end()) {
      if (!it->second.isObject()) {
        throw BistroException("'filters' must be an object for ", name);
      }
      for (const auto& pair : it->second.items()) {
        const auto& level = pair.first.asString().toStdString();
        const int level_id = config.levels.lookup(level);
        if (level_id == StringTable::NotFound) {
          throw BistroException("Invalid level in filters: ", level);
        }
        filters_[level_id] = JobFilters(pair.second);
      }
    }

    detail::parseKillOrphanTasksAfter(d, &killOrphanTasksAfter_);

    if (auto* ptr = d.get_ptr("version_id")) {
      if (!ptr->isInt()) {
        throw std::runtime_error("'version_id' must be an integer");
      }
      versionID_ = ptr->getInt();
    }

    if (const auto* host_level_ptr = d.get_ptr("level_for_host_placement")) {
      if (!host_level_ptr->isString()) {
        throw BistroException(
          "'level_for_host_placement' must be a string for ", name
        );
      }
      const auto& str_host_level =
        host_level_ptr->asString().toStdString();
      int host_level = config.levels.lookup(str_host_level);
      if (host_level == StringTable::NotFound) {
        throw BistroException(
          "Bad level_for_host_placement: ", str_host_level
        );
      }
      levelForHostPlacement_ = host_level;
    }

    if (const auto* host_ptr = d.get_ptr("host_placement")) {
      if (!host_ptr->isString()) {
        throw BistroException("'host_placement' must be a string for ", name);
      }
      hostPlacement_ = host_ptr->asString().toStdString();
      if (!hostPlacement_.empty()
          && levelForHostPlacement_ != StringTable::NotFound) {
        throw BistroException(
          "It makes no sense to specify both 'level_for_host_placement' and "
          "'host_placement'"
        );
      }
    }

    it = d.find("create_time");
    // Silently ignore non-integers because this isn't critical configuration
    if (it != d.items().end() && it->second.isInt()) {
      createTime_ = it->second.asInt();
    }

    it = d.find("modify_time");
    // Silently ignore non-integers because this isn't critical configuration
    if (it != d.items().end() && it->second.isInt()) {
      modifyTime_ = it->second.asInt();
    }

    // We don't check that the job names in depends_on are valid jobs.
    // If an invalid job is specified here, or if there is circular dependency,
    // this job can never run.
    it = d.find("depends_on");
    if (it != d.items().end()) {
      if (!it->second.isArray()) {
        throw BistroException("'depends_on' must be an array for ", name);
      }
      for (const auto& job_name : it->second) {
        dependsOn_.push_back(static_cast<ID>(JobNameTable->insert(
          job_name.asString().toStdString()
        )));
      }
    }
  } catch (const exception& e) {
    LOG(ERROR) << "Error creating job: " << e.what();
    error_ = e.what();
    enabled_ = false;
  }
}

/*
 * Contract: return the job's entire configuration (without the name), only
 * omitting trivially empty fields.  In particular, it's not ok to omit a
 * field just because it equals the deployment-wide default.
 *
 * Also remember to update ThriftConversion when you're adding fields!
 */
dynamic Job::toDynamic(const Config& parent_config) const {
  dynamic config = dynamic::object
    ("enabled", enabled_)
    ("owner", owner_)
    // Output Config::defaultJobConfig overlaid with the Job's initial
    // "config" key.  This has the effect that any write-back to the
    // ConfigLoader makes all of the default job settings explicit.
    ("config", config_)
    ("level_for_tasks", parent_config.levels.lookup(levelForTasks_))
    ("priority", priority_);
  {
    dynamic filters_obj = dynamic::object;
    for (int i = 0; i < filters_.size(); ++i) {
      if (!filters_[i].isEmpty()) {
        auto d = filters_[i].toDynamic();
        if (!d.empty()) {
          filters_obj[parent_config.levels.lookup(i)] = std::move(d);
        }
      }
    }
    if (!filters_obj.empty()) {
      config["filters"] = std::move(filters_obj);
    }
  }
  // Always output this, since it's affected by both Config and command-line
  // args, and omitting it would thus have weird edge cases.
  if (killOrphanTasksAfter_.hasValue()) {
    config["kill_orphan_tasks_after_sec"] =
      0.001 * killOrphanTasksAfter_->count();
  } else {
    config["kill_orphan_tasks_after_sec"] = false;
  }
  if (versionID_ >= 0) {
    config["version_id"] = versionID_;
  }
  if (levelForHostPlacement_ != StringTable::NotFound) {
    config["level_for_host_placement"] =
      parent_config.levels.lookup(levelForHostPlacement_);
  }
  if (!hostPlacement_.empty()) {
    config["host_placement"] = hostPlacement_;
  }
  {
    dynamic resources_obj = dynamic::object;
    for (const auto& resource_ids : parent_config.levelIDToResourceID) {
      for (auto resource_id : resource_ids) {
        // No point in reporting resources that the job doesn't use.
        if (auto num = resources_[resource_id]) {
          resources_obj[parent_config.resourceNames.lookup(resource_id)] = num;
        }
      }
    }
    if (!resources_obj.empty()) {
      config["resources"] = std::move(resources_obj);
    }
  }
  // Most of the time this equals parent_config.defaultBackoffSettings, but
  // we emit it anyway, because ambiguity is much worse than redundancy.
  config["backoff"] = backoffSettings_.toDynamic();
  if (!dependsOn_.empty()) {
    dynamic depends_on = {};
    for (auto job_id : dependsOn_) {
      depends_on.push_back(JobNameTable->lookup(job_id));
    }
    config["depends_on"] = depends_on;
  }
  if (createTime_ > 0) {
    config["create_time"] = createTime_;
  }
  if (modifyTime_ > 0) {
    config["modify_time"] = modifyTime_;
  }
  if (!error_.empty()) {
    config["error"] = error_;
  }
  return config;
}

string Job::requiredHostForTask(const Node& node) const {
  if (!hostPlacement_.empty()) {
    return hostPlacement_;
  }
  if (levelForHostPlacement_ == StringTable::NotFound) {
    return "";
  }
  for (const auto& n : node.traverseUp()) {
    if (n.level() == levelForHostPlacement_) {
      return n.name();
    }
  }
  return "";
}

Job::ShouldRun Job::shouldRunOn(const Node& node) const {
  for (const auto& n : node.traverseUp()) {
    if (!n.enabled()) {
      return ShouldRun::NoDisabled;
    }
    if (!filters_[n.level()].doesPass(name_, n)) {
      return ShouldRun::NoAvoided;
    }
  }
  return ShouldRun::Yes;
}

}}
