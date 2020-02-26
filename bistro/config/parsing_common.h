/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

/**
 * Parsers (and their toDynamic counterparts) used by both Config & Job.
 */

#include <chrono>
#include <folly/experimental/DynamicParser.h>
#include <folly/Optional.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/utils/SymbolTable.h"

namespace facebook { namespace bistro {

// Top-level fields
constexpr folly::StringPiece kEnabled = "enabled";
constexpr folly::StringPiece kBackoff = "backoff";
constexpr folly::StringPiece kNodes = "nodes";
constexpr folly::StringPiece kLevelForTasks = "level_for_tasks";
constexpr folly::StringPiece kKillOrphanTasksAfterSec =
  "kill_orphan_tasks_after_sec";
// Task subprocess
constexpr folly::StringPiece kTaskSubprocess = "task_subprocess";
constexpr folly::StringPiece kPollMs = "poll_ms";
constexpr folly::StringPiece kMaxLogLinesPerPollInterval =
  "max_log_lines_per_poll_interval";
constexpr folly::StringPiece kParentDeathSignal = "parent_death_signal";
constexpr folly::StringPiece kProcessGroupLeader = "process_group_leader";
constexpr folly::StringPiece kUseCanaryPipe = "use_canary_pipe";
// CGroups
constexpr folly::StringPiece kCGroups = "cgroups";
constexpr folly::StringPiece kRoot = "root";
constexpr folly::StringPiece kSlice = "slice";
constexpr folly::StringPiece kSubsystems = "subsystems";
constexpr folly::StringPiece kKillWithoutFreezer = "kill_without_freezer";
// Kill request
constexpr folly::StringPiece kKillSubprocess = "kill_subprocess";
constexpr folly::StringPiece kMethod = "method";
constexpr folly::StringPiece kTermWaitKill = "term_wait_kill";
constexpr folly::StringPiece kTerm = "term";
constexpr folly::StringPiece kKill = "kill";
constexpr folly::StringPiece kKillWaitMs = "kill_wait_ms";


template <typename T>
int checkedLookup(const StringTable& table, T key) {
  int id = table.lookup(key);
  if (id == StringTable::NotFound) {
    throw std::runtime_error("Unknown identifier");
  }
  return id;
}


inline folly::dynamic killOrphanTasksAfterToDynamic(
    folly::Optional<std::chrono::milliseconds> v) {
  if (v.has_value()) {
    return 0.001 * v->count();
  }
  return false;
}
// Deliberately does NOT alter the output value if this key is not set. This
// allows FLAGS_kill_job_if_disabled to work as advertised in Config.cpp.
void parseKillOrphanTasksAfter(
  folly::DynamicParser* p,
  folly::Optional<std::chrono::milliseconds>* maybe_kill_orphans
);


folly::dynamic taskSubprocessOptionsToDynamic(
  const cpp2::TaskSubprocessOptions& opts
);
void parseTaskSubprocessOptions(
  folly::DynamicParser* p,
  cpp2::TaskSubprocessOptions* opts
);


folly::dynamic killRequestToDynamic(const cpp2::KillRequest& req);
void parseKillRequest(folly::DynamicParser* p, cpp2::KillRequest* req);

}}  // namespace facebook::bistro
