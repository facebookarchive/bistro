/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/thrift/ThriftConversion.h"

#include "bistro/bistro/config/utils.h"
#include <folly/json.h>

namespace facebook { namespace bistro {

using namespace std;
using folly::dynamic;

namespace {

unordered_set<string> asSet(const dynamic* d) {
  unordered_set<string> s;
  if (d) {
    for (const auto& i : *d) {
      s.insert(i.asString().toStdString());
    }
  }
  return s;
}

}

dynamic toDynamic(const cpp2::BistroJobConfigFilters& f) {
  dynamic d = dynamic::object;
  if (!f.whitelist.empty()) {
    d["whitelist"] = dynamic(f.whitelist.begin(), f.whitelist.end());
  }
  if (!f.whitelistRegex.empty()) {
    d["whitelist_regex"] = f.whitelistRegex;
  }
  if (!f.blacklist.empty()) {
    d["blacklist"] = dynamic(f.blacklist.begin(), f.blacklist.end());
  }
  if (!f.blacklistRegex.empty()) {
    d["blacklist_regex"] = f.blacklistRegex;
  }
  if (f.fractionOfNodes != 1.0) {
    d["fraction_of_nodes"] = f.fractionOfNodes;
  }
  if (!f.tagWhitelist.empty()) {
    d["tag_whitelist"] = dynamic(f.tagWhitelist.begin(), f.tagWhitelist.end());
  }
  return d;
}

cpp2::BistroJobConfigFilters toThrift(const dynamic& d) {
  return cpp2::BistroJobConfigFilters(
    apache::thrift::FRAGILE,
    asSet(d.get_ptr("whitelist")),
    d.getDefault("whitelist_regex", "").asString().toStdString(),
    asSet(d.get_ptr("blacklist")),
    d.getDefault("blacklist_regex", "").asString().toStdString(),
    d.getDefault("fraction_of_nodes", 1.0).asDouble(),
    asSet(d.get_ptr("tag_whitelist"))
  );
}

dynamic toDynamic(const cpp2::BistroJobConfig& c) {
  dynamic filters_obj = dynamic::object;
  dynamic resources_obj = dynamic::object;
  for (const auto& pair : c.filters) {
    filters_obj[pair.first] = toDynamic(pair.second);
  }
  for (const auto& pair : c.resources) {
    resources_obj[pair.first] = pair.second;
  }
  dynamic config = dynamic::object
    ("name", c.name)
    ("enabled", c.enabled)
    ("owner", c.owner)
    ("config", folly::parseJson(c.config))
    ("priority", c.priority)
  ;
  if (!filters_obj.empty()) {
    config["filters"] = filters_obj;
  }
  if (c.__isset.killOrphanTasksAfterSec) {
    config["kill_orphan_tasks_after_sec"] = c.killOrphanTasksAfterSec;
  } else {
    config["kill_orphan_tasks_after_sec"] = false;
  }
  if (c.versionID != -1) {
    config["version_id"] = c.versionID;
  }
  if (!c.levelForHostPlacement.empty()) {
    config["level_for_host_placement"] = c.levelForHostPlacement;
  }
  if (!c.hostPlacement.empty()) {
    config["host_placement"] = c.hostPlacement;
  }
  if (!resources_obj.empty()) {
    config["resources"] = resources_obj;
  }
  if (!c.error.empty()) {
    config["error"] = c.error;
  }
  if (!c.backoffValues.empty()) {
    config["backoff"] = dynamic(c.backoffValues.begin(), c.backoffValues.end());
    if (c.backoffRepeat) {
      config["backoff"].push_back("repeat");
    }
  }
  if (!c.dependsOn.empty()) {
    config["depends_on"] = dynamic(c.dependsOn.begin(), c.dependsOn.end());
  }
  if (!c.levelForTasks.empty()) {
    config["level_for_tasks"] = c.levelForTasks;
  }
  return config;
}

cpp2::BistroJobConfig toThrift(const std::string& name, const dynamic& d) {
  cpp2::BistroJobConfig config;
  config.name = name;
  config.enabled = d.getDefault("enabled", false).asBool();
  config.owner = d.getDefault("owner", "").asString().toStdString();
  config.priority = d.getDefault("priority", 1.0).asDouble();
  config.config = folly::toJson(d.getDefault("config", "")).toStdString();
  config.error = d.getDefault("error", "").asString().toStdString();
  config.createTime = d.getDefault("create_time", 0).asInt();
  config.modifyTime = d.getDefault("modify_time", 0).asInt();
  config.levelForTasks =
    d.getDefault("level_for_tasks", "").asString().toStdString();
  if (auto* p = d.get_ptr("resources")) {
    for (const auto& pair : p->items()) {
      config.resources[pair.first.asString().toStdString()] =
        pair.second.asInt();
    }
  }
  if (auto* p = d.get_ptr("filters")) {
    for (const auto& pair : p->items()) {
      config.filters[pair.first.asString().toStdString()] =
        toThrift(pair.second);
    }
  }
  if (auto* p = d.get_ptr("kill_orphan_tasks_after_sec")) {
    if (p->isBool()) {
      config.__isset.killOrphanTasksAfterSec = p->asBool();
      config.killOrphanTasksAfterSec = 0;
    } else if (p->isNumber()) {
      config.__isset.killOrphanTasksAfterSec = true;
      config.killOrphanTasksAfterSec = p->asDouble();
    } else {
      throw std::runtime_error(folly::to<std::string>(
        "Invalid kill_orphan_tasks_after_sec", folly::toJson(*p)
      ));
    }
  }
  if (auto *p = d.get_ptr("version_id")) {
    if (!p->isInt()) {
      throw std::runtime_error("'version_id' must be an integer");
    }
    config.versionID = p->asInt();
  }
  config.levelForHostPlacement =
    d.getDefault("level_for_host_placement", "").asString().toStdString();
  config.hostPlacement =
    d.getDefault("host_placement", "").asString().toStdString();
  if (auto* p = d.get_ptr("backoff")) {
    for (const auto& d : *p) {
      if (d.isInt()) {
        config.backoffValues.push_back(d.asInt());
      } else if (d.isString() && d.asString() == "repeat") {
        config.backoffRepeat = true;
      }
    }
  }
  if (auto *p = d.get_ptr("depends_on")) {
    for (const auto& job_name : *p) {
      config.dependsOn.push_back(job_name.asString().toStdString());
    }
  }
  return config;
}

}}
