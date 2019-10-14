/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/Job.h"

#include <folly/experimental/DynamicParser.h>
#include <folly/json.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/parsing_common.h"

namespace facebook { namespace bistro {

using dynamic = folly::dynamic;

folly::Synchronized<StringTable> Job::JobNameTable =
  folly::Synchronized<StringTable>();

Job::Job(const Config& config, const std::string& name, const dynamic& d,
    JobFilters::NodeDoesPassCob filter_cb)
  // IMPORTANT: The initializers MUST NOT THROW. Use them to set defaults
  // (or if possible, default-initializers in Job.h), and then use
  // DynamicParser in the body for any parsing and validation.
  //
  // It is critical to use DynamicParser down to the finest possible level
  // of granularity (any configuration part that can be changed
  // independently, usually leaf nodes in the JSON tree).  Otherwise, the
  // following sequence of user requests is almost certainly going to
  // violate their expectations:
  //   1) saveJob(partially invalid jobspec)
  //   2) saveJob(manualCorrection(loadJob()))
  //
  // Had we failed to parse, record, and convert toDynamic() all the valid
  // parts of the jobspec, the "corrected" jobspec would be missing parts of
  // the original user intent.
  : id_(Job::JobNameTable->insert(name)),
    name_(name),
    levelForTasks_(config.levelForTasks),
    resources_(config.defaultJobResources),
    config_(dynamic::object),
    filters_(config.levels.size()),
    backoffSettings_(config.defaultBackoffSettings),
    killOrphanTasksAfter_(config.killOrphanTasksAfter),
    taskSubprocessOptions_(config.taskSubprocessOptions),
    killRequest_(config.killRequest),
    errors_(dynamic::object()) {

  folly::DynamicParser p(folly::DynamicParser::OnError::RECORD, &d);

  p.optional(kEnabled, [&](bool b) { enabled_ = b; });
  p.required("owner", [&](std::string&& s) { owner_ = std::move(s); });
  p.optional(kLevelForTasks, [&](const std::string& level) {
    levelForTasks_ = checkedLookup(config.levels, level);
  });
  p.optional("priority", [&](double n) { priority_ = n; });
  p.optional("resources", [&]() {
    p.objectItems([&](std::string r_name, int64_t amount) {
      resources_[checkedLookup(config.resourceNames, r_name)] = amount;
    });
  });
  p.optional("config", [&](const dynamic& v) {
    // This requirement is historical, it could be okay to remove it.
    if (!v.isObject()) { throw BistroException("Must be an object"); }
    config_ = v;
  });
  p.optional("filters", [&]() {
    p.objectItems([&](std::string level, const dynamic& v) {
      // On error, a user must redo the whole filter. This seems unlikely to
      // cause confusion or data-loss.  If not, use `p` to make JobFilters.
      filters_[checkedLookup(config.levels, level)] = JobFilters(v, filter_cb);
    });
  });
  p.optional("level_for_host_placement", [&](const std::string& level) {
    levelForHostPlacement_ = checkedLookup(config.levels, level);
  });
  p.optional("host_placement", [&](std::string&& s) {
    hostPlacement_ = std::move(s);
    // If both host_placement and level_for_host_placement are set, the
    // former prevails since it is more specific.  This is not an error.
  });
  p.optional(kBackoff, [&](const dynamic& v) {
    backoffSettings_ = JobBackoffSettings(v);  // Same note as for JobFilters
  });
  // We don't check that the job names in depends_on are valid jobs.  If an
  // invalid job is specified here, or if there is circular dependency, this
  // job can never run.
  p.optional("depends_on", [&]() {
    p.arrayItems([&](const std::string& job_name) {
      dependsOn_.push_back(static_cast<ID>(JobNameTable->insert(job_name)));
    });
  });
  p.optional("create_time", [&](int64_t n) { createTime_ = n; });
  p.optional("modify_time", [&](int64_t n) { modifyTime_ = n; });
  parseKillOrphanTasksAfter(&p, &killOrphanTasksAfter_);
  parseTaskSubprocessOptions(&p, &taskSubprocessOptions_);
  parseKillRequest(&p, &killRequest_);
  p.optional(kCommand, [&]() {
    p.arrayItems([&](std::string&& s) {
      command_.emplace_back(std::move(s));
    });
  });
  p.optional("version_id", [&](int64_t n) { versionID_ = n; });

  // Record any pre-existing errors (in a nested "errors" key).
  p.optional("errors", [&]() {
    throw BistroException("Pre-existing errors");
  });
  // Now that we are done parsing, it may turn out that the only kind of
  // errors are the pre-existing ones.  If so, remove the pointless nesting.
  errors_ = p.releaseErrors();
  const auto& inner_errors = errors_.getDefault("nested").getDefault("errors");
  const dynamic only_inner_errors =
    dynamic::object("nested", dynamic::object("errors", inner_errors));
  if (errors_ == only_inner_errors) {
    errors_ = inner_errors.getDefault("value");
  }
  if (!errors_.empty()) {
    LOG(ERROR) << "Error creating job " << name << ": "
      << folly::toPrettyJson(errors_);
  }
}

/*
 * Contract: return the job's entire configuration (without the name), only
 * omitting trivially empty fields.  In particular, any field whose value is
 * inherited from Config MUST be exported.  If a Job-only field has a
 * non-trivial default which could CONCEIVABLY have been different, you
 * should also export it explicitly.
 *
 * Also remember to update ThriftConversion when you're adding fields!
 *
 * == Rationale ==
 *
 * It would be far simpler to just retain the original "dynamic" that we
 * parsed.  In fact, Bistro should probably also have this feature, so that
 * a save-load cycle on a job does not destroy this inheritance.
 *
 * However, for applications which are trying to programmatically understand
 * the state of jobs, it is crucial to be able to get the "true" view of a
 * job, without having to unravel Bistro's inheritance and defaulting.
 */
dynamic Job::toDynamic(const Config& parent_config) const {
  //
  // First, the fields that are ALWAYS exported. If in doubt, err on the
  // side of always exporting -- see the docblock for the "why?".
  //
  dynamic config = dynamic::object
    (kEnabled, enabled_)
    ("owner", owner_)
    (kLevelForTasks, parent_config.levels.lookup(levelForTasks_))
    ("priority", priority_)
    ("config", config_)
    (kBackoff, backoffSettings_.toDynamic())
    (kKillOrphanTasksAfterSec,
      killOrphanTasksAfterToDynamic(killOrphanTasksAfter_))
    (kTaskSubprocess, taskSubprocessOptionsToDynamic(taskSubprocessOptions_))
    (kKillSubprocess, killRequestToDynamic(killRequest_))
  ;
  {  // As per docblock rule, always export "resources" (inherited from Config)
    dynamic resources_obj = dynamic::object;
    for (const auto& resource_ids : parent_config.levelIDToResourceID) {
      for (auto resource_id : resource_ids) {
        resources_obj[parent_config.resourceNames.lookup(resource_id)] =
          resources_[resource_id];
      }
    }
    if (!resources_obj.empty()) {  // Ok to omit if no resources exist.
      config["resources"] = std::move(resources_obj);
    }
  }
  //
  // The following fields  have trivial, never-changing defaults, so we can
  // safely omit them when empty.
  //
  {  // scope for "filters"
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
  if (levelForHostPlacement_ != StringTable::NotFound) {
    config["level_for_host_placement"] =
      parent_config.levels.lookup(levelForHostPlacement_);
  }
  if (!hostPlacement_.empty()) {
    config["host_placement"] = hostPlacement_;
  }
  if (!dependsOn_.empty()) {
    dynamic depends_on = dynamic::array;
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
  if (!command_.empty()) {
    config[kCommand] = folly::dynamic(command_.begin(), command_.end());
  }
  if (versionID_ >= 0) {
    config["version_id"] = versionID_;
  }
  if (!errors_.empty()) {
    config["errors"] = errors_;
  }
  return config;
}

std::string Job::requiredHostForTask(const Node& node) const {
  // Specific host placement takes precedence over placement by level.
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
