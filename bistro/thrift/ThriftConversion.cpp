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
  if (!f.whitelist_ref()->empty()) {
    d["whitelist"] =
        dynamic(f.whitelist_ref()->begin(), f.whitelist_ref()->end());
  }
  if (!f.whitelistRegex_ref()->empty()) {
    d["whitelist_regex"] = *f.whitelistRegex_ref();
  }
  if (!f.blacklist_ref()->empty()) {
    d["blacklist"] =
        dynamic(f.blacklist_ref()->begin(), f.blacklist_ref()->end());
  }
  if (!f.blacklistRegex_ref()->empty()) {
    d["blacklist_regex"] = *f.blacklistRegex_ref();
  }
  if (*f.fractionOfNodes_ref() != 1.0) {
    d["fraction_of_nodes"] = *f.fractionOfNodes_ref();
  }
  if (!f.tagWhitelist_ref()->empty()) {
    d["tag_whitelist"] =
        dynamic(f.tagWhitelist_ref()->begin(), f.tagWhitelist_ref()->end());
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
  for (const auto& pair : *c.filters_ref()) {
    filters_obj[pair.first] = toDynamic(pair.second);
  }
  for (const auto& pair : *c.resources_ref()) {
    resources_obj[pair.first] = pair.second;
  }
  dynamic config =
      dynamic::object("name", *c.name_ref())("enabled", *c.enabled_ref())(
          "owner", *c.owner_ref())("config", folly::parseJson(*c.config_ref()))(
          "priority", *c.priority_ref());
  if (!filters_obj.empty()) {
    config["filters"] = filters_obj;
  }
  if (c.killOrphanTasksAfterSec_ref()) {
    config["kill_orphan_tasks_after_sec"] =
        c.killOrphanTasksAfterSec_ref().value_unchecked();
  } else {
    config["kill_orphan_tasks_after_sec"] = false;
  }
  config[kTaskSubprocess] =
      taskSubprocessOptionsToDynamic(*c.taskSubprocessOptions_ref());
  config[kKillSubprocess] = killRequestToDynamic(*c.killRequest_ref());
  if (*c.versionID_ref() != -1) {
    config["version_id"] = *c.versionID_ref();
  }
  if (!c.levelForHostPlacement_ref()->empty()) {
    config["level_for_host_placement"] = *c.levelForHostPlacement_ref();
  }
  if (!c.hostPlacement_ref()->empty()) {
    config["host_placement"] = *c.hostPlacement_ref();
  }
  if (!resources_obj.empty()) {
    config["resources"] = resources_obj;
  }
  if (!c.error_ref()->empty()) {
    config["errors"] = folly::parseJson(*c.error_ref());
  }
  if (!c.backoffValues_ref()->empty()) {
    config["backoff"] =
        dynamic(c.backoffValues_ref()->begin(), c.backoffValues_ref()->end());
    if (*c.backoffRepeat_ref()) {
      config["backoff"].push_back("repeat");
    }
  }
  if (!c.dependsOn_ref()->empty()) {
    config["depends_on"] =
        dynamic(c.dependsOn_ref()->begin(), c.dependsOn_ref()->end());
  }
  if (!c.levelForTasks_ref()->empty()) {
    config["level_for_tasks"] = *c.levelForTasks_ref();
  }
  return config;
}

cpp2::BistroJobConfig toThrift(const std::string& name, const dynamic& d) {
  // Defaults for "optional" fields are set by the .thrift file.
  cpp2::BistroJobConfig config;
  *config.name_ref() = name;
  folly::DynamicParser p(folly::DynamicParser::OnError::RECORD, &d);
  p.optional("enabled", [&](bool b) { *config.enabled_ref() = b; });
  p.optional(
      "owner", [&](std::string&& s) { *config.owner_ref() = std::move(s); });
  p.optional("priority", [&](double p) { *config.priority_ref() = p; });
  *config.config_ref() = "{}"; // No default in .thrift file :/
  p.optional(
      "config", [&]() { *config.config_ref() = folly::toJson(p.value()); });
  // Since this toThrift is meant to be used on the output of toDynamic, it
  // is not an error to have something in "errors".
  p.optional(
      "errors", [&]() { *config.error_ref() = folly::toJson(p.value()); });
  p.optional("create_time", [&](int64_t n) { *config.createTime_ref() = n; });
  p.optional("modify_time", [&](int64_t n) { *config.modifyTime_ref() = n; });
  p.optional("level_for_tasks", [&](std::string&& s) {
    *config.levelForTasks_ref() = std::move(s);
  });
  p.optional("resources", [&]() {
    p.objectItems([&](std::string&& resourceName, int64_t amount) {
      config.resources_ref()[resourceName] = amount;
    });
  });
  p.optional("filters", [&]() {
    p.objectItems([&](std::string&& filterName, const folly::dynamic& v) {
      config.filters_ref()[filterName] = toThrift(v);
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
  parseTaskSubprocessOptions(&p, &(*config.taskSubprocessOptions_ref()));
  parseKillRequest(&p, &(*config.killRequest_ref()));
  p.optional("version_id", [&](int64_t n) { *config.versionID_ref() = n; });
  p.optional("level_for_host_placement", [&](std::string&& s) {
    *config.levelForHostPlacement_ref() = std::move(s);
  });
  p.optional("host_placement", [&](std::string&& s) {
    *config.hostPlacement_ref() = std::move(s);
  });
  p.optional("backoff", [&]() {
    p.arrayItems([&](const folly::dynamic& v) {
      if (v.isInt()) {
        config.backoffValues_ref()->push_back(v.asInt());
      } else if (v.isString() && v.asString() == "repeat") {
        *config.backoffRepeat_ref() = true;
      } else if (!v.isString() || v.asString() != "fail") {
        throw std::runtime_error("Unknown backoff value");
      }
    });
  });
  p.optional("depends_on", [&]() {
    p.arrayItems([&](std::string&& dep_name) {
      config.dependsOn_ref()->emplace_back(std::move(dep_name));
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
