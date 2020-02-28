/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/parsing_common.h"

namespace facebook { namespace bistro {

void parseKillOrphanTasksAfter(
    folly::DynamicParser* p,
    folly::Optional<std::chrono::milliseconds>* maybe_kill_orphans) {
  // If not set: Config defaults to FLAGS_kill_job_if_disabled, Job to none.
  p->optional(kKillOrphanTasksAfterSec, [&](const folly::dynamic& val) {
    maybe_kill_orphans->reset(); // 'false' or negative number => do not kill
    if (val.isBool()) {
      if (val.asBool()) {
        *maybe_kill_orphans = std::chrono::milliseconds(0);
      }  // False: do not kill
    } else if (val.isNumber()) {
      auto sec = val.asDouble();
      if (sec >= 0.0) {
        *maybe_kill_orphans =
          std::chrono::milliseconds(static_cast<int>(1000 * sec));
      }  // Negative: do not kill
    } else {
      throw std::runtime_error("Must be a number or a boolean");
    }
  });
}

namespace {
folly::dynamic cgroupOptionsToDynamic(const cpp2::CGroupOptions& cgopts) {
  return folly::dynamic::object
    (kRoot, cgopts.root)
    (kSlice, cgopts.slice)
    (kSubsystems,
     folly::dynamic(cgopts.subsystems.begin(), cgopts.subsystems.end()))
    (kKillWithoutFreezer, cgopts.killWithoutFreezer);
}
}  // anonymous namespace

folly::dynamic taskSubprocessOptionsToDynamic(
    const cpp2::TaskSubprocessOptions& opts) {
  return folly::dynamic::object
    (kPollMs, opts.pollMs)
    (kMaxLogLinesPerPollInterval, opts.maxLogLinesPerPollInterval)
    (kParentDeathSignal, opts.parentDeathSignal)
    (kProcessGroupLeader, opts.processGroupLeader)
    (kUseCanaryPipe, opts.useCanaryPipe)
    (kCGroups, cgroupOptionsToDynamic(opts.cgroupOptions));
}

void parseTaskSubprocessOptions(
    folly::DynamicParser* p,
    cpp2::TaskSubprocessOptions* opts) {
  p->optional(kTaskSubprocess, [&]() {
    p->optional(kPollMs, [&](int64_t n) { opts->pollMs = n; });
    p->optional(kMaxLogLinesPerPollInterval, [&](int64_t n) {
      opts->maxLogLinesPerPollInterval = n;
    });
    p->optional(kParentDeathSignal, [&](int64_t n) {
      opts->parentDeathSignal = n;
    });
    p->optional(kProcessGroupLeader, [&](bool b) {
      opts->processGroupLeader = b;
    });
    p->optional(kUseCanaryPipe, [&](bool b) {
      opts->useCanaryPipe = b;
    });
    p->optional(kCGroups, [&]() {
      auto& cgopts = opts->cgroupOptions;
      p->optional(kRoot, [&](std::string&& s) { cgopts.root = std::move(s); });
      p->optional(kSlice, [&](std::string&& s) {
        cgopts.slice = std::move(s);
      });
      p->optional(kSubsystems, [&]() {
        // This is required since Job will just parse **on top** of whatever
        // it inherits from Config.  With multiple save-load cycles, the
        // Config's list of subsystems would get copied to the job, then and
        // grow by one extra copy with each cycle.
        cgopts.subsystems.clear();
        p->arrayItems([&](std::string&& s) {
          cgopts.subsystems.emplace_back(std::move(s));
        });
      });
      p->optional(kKillWithoutFreezer, [&](bool b) {
        cgopts.killWithoutFreezer = b;
      });
      // cpuShares and memoryLimitInBytes will be populated on a
      // per-task basis, based on their worker resources using
      // PhysicalResourceConfig.  unitTestCreateFiles is for tests.
    });
  });
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
          throw std::runtime_error(folly::to<std::string>(
            "Unknown KillMethod ", static_cast<int>(req.method)
          ));
      }
    }())
    (kKillWaitMs, req.killWaitMs);
}

void parseKillRequest(folly::DynamicParser* p, cpp2::KillRequest* req) {
  p->optional(kKillSubprocess, [&]() {
    p->optional(kMethod, [&](const std::string& s) {
      if (s == kTermWaitKill) {
        req->method = cpp2::KillMethod::TERM_WAIT_KILL;
      } else if (s == kTerm) {
        req->method = cpp2::KillMethod::TERM;
      } else if (s == kKill) {
        req->method = cpp2::KillMethod::KILL;
      } else {
        throw std::runtime_error("Unknown KillMethod");
      }
    });
    p->optional(kKillWaitMs, [&](int64_t n) { req->killWaitMs = n; });
  });
}

}}  // namespace facebook::bistro
