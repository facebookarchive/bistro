/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/thrift/ThriftConversion.h"

#include <folly/json.h>
#include <folly/experimental/DynamicParser.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/parsing_common.h"

namespace facebook { namespace bistro {

using namespace std;
using folly::dynamic;

namespace {

unordered_set<string> asSet(const dynamic* d) {
  unordered_set<string> s;
  if (d) {
    for (const auto& i : *d) {
      s.insert(i.asString());
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
    d.getDefault("whitelist_regex", "").asString(),
    asSet(d.get_ptr("blacklist")),
    d.getDefault("blacklist_regex", "").asString(),
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
    config["kill_orphan_tasks_after_sec"] =
        c.killOrphanTasksAfterSec_ref().value_unchecked();
  } else {
    config["kill_orphan_tasks_after_sec"] = false;
  }
  config[kTaskSubprocess] =
    taskSubprocessOptionsToDynamic(c.taskSubprocessOptions);
  config[kKillSubprocess] = killRequestToDynamic(c.killRequest);
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
    config["errors"] = folly::parseJson(c.error);
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
  // Defaults for "optional" fields are set by the .thrift file.
  cpp2::BistroJobConfig config;
  config.name = name;
  folly::DynamicParser p(folly::DynamicParser::OnError::RECORD, &d);
  p.optional("enabled", [&](bool b) { config.enabled = b; });
  p.optional("owner", [&](std::string&& s) { config.owner = std::move(s); });
  p.optional("priority", [&](double p) { config.priority = p; });
  config.config = "{}";  // No default in .thrift file :/
  p.optional("config", [&]() { config.config = folly::toJson(p.value()); });
  // Since this toThrift is meant to be used on the output of toDynamic, it
  // is not an error to have something in "errors".
  p.optional("errors", [&]() { config.error = folly::toJson(p.value()); });
  p.optional("create_time", [&](int64_t n) { config.createTime = n; });
  p.optional("modify_time", [&](int64_t n) { config.modifyTime = n; });
  p.optional("level_for_tasks", [&](std::string&& s) {
    config.levelForTasks = std::move(s);
  });
  p.optional("resources", [&]() {
    p.objectItems([&](std::string&& resourceName, int64_t amount) {
      config.resources[resourceName] = amount;
    });
  });
  p.optional("filters", [&]() {
    p.objectItems([&](std::string&& filterName, const folly::dynamic& v) {
      config.filters[filterName] = toThrift(v);
    });
  });
  {
    folly::Optional<std::chrono::milliseconds> maybe_kill_ms;
    parseKillOrphanTasksAfter(&p, &maybe_kill_ms);
    if (maybe_kill_ms.has_value()) {
      config.killOrphanTasksAfterSec_ref() = 0.001 * maybe_kill_ms->count();
    } else {
      config.__isset.killOrphanTasksAfterSec = false;
    }
  }
  parseTaskSubprocessOptions(&p, &config.taskSubprocessOptions);
  parseKillRequest(&p, &config.killRequest);
  p.optional("version_id", [&](int64_t n) { config.versionID = n; });
  p.optional("level_for_host_placement", [&](std::string&& s) {
    config.levelForHostPlacement = std::move(s);
  });
  p.optional("host_placement", [&](std::string&& s) {
    config.hostPlacement = std::move(s);
  });
  p.optional("backoff", [&]() {
    p.arrayItems([&](const folly::dynamic& v) {
      if (v.isInt()) {
        config.backoffValues.push_back(v.asInt());
      } else if (v.isString() && v.asString() == "repeat") {
        config.backoffRepeat = true;
      } else if (!v.isString() || v.asString() != "fail") {
        throw std::runtime_error("Unknown backoff value");
      }
    });
  });
  p.optional("depends_on", [&]() {
    p.arrayItems([&](std::string&& dep_name) {
      config.dependsOn.emplace_back(std::move(dep_name));
    });
  });
  auto errors = p.releaseErrors();
  if (!errors.empty()) {
    // Should never happen:
    throw BistroException(
      "Failed to produce BistroJobConfig: ", folly::toJson(errors)
    );
  }
  return config;
}

}}
