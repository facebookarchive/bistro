/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/processes/AsyncCGroupReaper.h"

#include <signal.h>
#include <boost/filesystem.hpp>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/gen/File.h>

#include "bistro/bistro/utils/Exception.h"

// Future: make some dedicated tests patterned on the integration tests with
// TestTaskSubprocessQueue, but less clunky, and deeper.

DEFINE_int32(
  cgroup_freeze_timeout_ms, 10000,
  "If a cgroup fails to freeze for this long, it will be thawed and re-frozen "
  "to try to mitigate a race inherent between OOM-notifier & freezer. If you "
  "set this too low, a freeze will never succeed, breaking cgroup killing."
);

namespace facebook { namespace bistro {

namespace {
// Make via asyncCGroupReaper below. Patterned on AsyncSubprocess.
class AsyncCGroupReaper : public folly::AsyncTimeout {
public:
  AsyncCGroupReaper(
    folly::EventBase* event_base,
    cpp2::CGroupOptions cgopts,
    std::string cgname,
    uint32_t min_wait_ms,
    uint32_t max_wait_ms
  ) : folly::AsyncTimeout(event_base),
      cgroupOpts_(std::move(cgopts)),
      cgroupName_(std::move(cgname)),
      minWaitMs_(min_wait_ms),
      maxWaitMs_(max_wait_ms),
      curWaitMs_(min_wait_ms) {}

  // Since Promise has a nontrivial destructor, it may be bad to `delete
  // this` in the constructor, possibly even "undefined behavior"-bad :)
  void initialize(folly::Future<folly::Unit>* empty_cgroups) {
    // DO NOT use objects with non-empty destructors here, we `delete this`.
    *empty_cgroups = emptyCGroups_.getFuture();
    // Reduce latency: check eagerly if there are no groups to wait for.
    if (auto nonempty_subsystem = findNonEmptySubsystem()) {
      // Make one kill attempt immediately, since it takes at least 2 calls
      // to actually signal the cgroup (1 to freeze, and 1 to signal).  In
      // the common case of "no D state tasks", this should cut down cgroup
      // kill latency to 1-5 minWaitMs_ intervals.
      workOnKillingCGroupTasks(*nonempty_subsystem);  // noexcept
      // The first delay is exempt from exponential backoff.
      myScheduleTimeout(minWaitMs_);
      // If AsyncCGroupReaper were created outside the EventBase thread, the
      // class could already be destroyed by timeoutExpired() by this point.
      return;  // Do not self-destruct.
    }
    emptyCGroups_.setValue();
    delete this;
    // Better do **nothing** else, `this` is gone.
  }

  void timeoutExpired() noexcept override {
    // DO NOT use objects with non-empty destructors here, we `delete this`.
    totalWaitMs_ += curWaitMs_;
    // Check before trying to kill. The other order is less efficient -- a
    // successful kill is not instant, so an immediate areCGroupEmpty()
    // check would usually fail anyway.
    if (auto nonempty_subsystem = findNonEmptySubsystem()) {
      LOG(WARNING) << "Trying to reap intransigent task with cgroup "
        << cgroupName_ << " for over " << totalWaitMs_ << " ms";
      workOnKillingCGroupTasks(*nonempty_subsystem);
      myScheduleTimeout(maxWaitMs_);  // Timeout exponentially grows to max.
      return;  // Not done yet, don't self-destruct.
    }
    LOG(WARNING) << "Intransigent task with cgroup " << cgroupName_
      << " exited after " << totalWaitMs_ << " ms";
    emptyCGroups_.setValue();
    delete this;
    // Better do **nothing** else, `this` is gone.
  }

private:
  void myScheduleTimeout(uint32_t ms) {
    curWaitMs_ = std::min(ms, curWaitMs_ << 2);  // 4x exponential backoff
    scheduleTimeout(curWaitMs_);
  }

  std::string cgroupDir(const std::string& subsystem) const {
    return (
      boost::filesystem::path(cgroupOpts_.root) / subsystem
        / cgroupOpts_.slice / cgroupName_
    ).native();
  }

  /**
   * Returns false if none of the task's cgroups have processes in them.
   * Removes empty cgroup directories, which is important when the system
   * `release_agent` reaper is not configured.
   */
  folly::Optional<std::string> findNonEmptySubsystem() noexcept {
    // While checking /cgroup.procs files, keep in mind that the kernel
    // `release_agent` can remove a cgroup from under us at any time.
    for (const auto& subsystem : cgroupOpts_.subsystems) {
      auto dir = cgroupDir(subsystem);
      boost::system::error_code ec;
      if (!boost::filesystem::is_directory(dir, ec) || ec) {
        continue;  // cgroup was likely already reaped
      }
      // Don't trust `filesystem::is_empty` since sysfs metadata is bogus.
      try {
        folly::File procs_file(dir + "/cgroup.procs");
        char c;  // Reading one byte is enough to prove non-emptiness.
        ssize_t bytes_read = folly::readFull(procs_file.fd(), &c, 1);
        if (bytes_read == 1) {
          if (c < '0' || c > '9') {
            LOG(WARNING) << dir << "/cgroup.procs starts with bad char: " << c;
          }
          return subsystem;  // At least one cgroup contains data.
        } else if (bytes_read == -1) {
          PLOG(WARNING) << dir << "/cgroup.procs is unreadable";
        } else if (bytes_read == 0) {
          // Empty cgroup, but the directory still exists -- try to reap it.
          if (boost::filesystem::remove(dir, ec) && !ec) {
            LOG(INFO) << "Removed empty cgroup " << dir;
          } else if (ec) {
            LOG(WARNING) << "Failed to remove empty cgroup: " << dir
              << ": " << ec.message();
          }  // else: no file or directory existed at `dir`, no error occurred.
        } else {
          LOG(FATAL) << "read() returned bad value: " << bytes_read;
        }
      } catch (const std::exception& ex) {
        // Maybe we raced the system `release_agent` to reap the directory?
        LOG(WARNING) << dir << "/cgroup.procs is unreadable: " << ex.what();
      }
      // Either the cgroup is empty, or a read error made us assume it's gone.
    }
    return folly::none;
  }

  // Helper for workOnKillingCGroupTasks.
  template <class... Args>
  void badFreezer(Args&&... args) noexcept {
    LOG(WARNING) << "Not sending SIGKILL to tasks in the " << cgroupName_
      << " cgroups, since the `freezer` subsystem"
      << folly::to<std::string>(std::forward<Args>(args)...)
      << ", which would make it easy to kill the wrong processes.";
  }

  // Helper for workOnKillingCGroupTasks.
  // Nothing to be done on error, so this does not have a return value.
  void freezerWrite(
      const std::string& freezer_state_path,
      const std::string& value) noexcept {
    if (!folly::writeFile(  // No O_CREAT. If the path is bad, append is safer.
      value, freezer_state_path.c_str(), O_WRONLY | O_APPEND
    )) {
      badFreezer("'s state is unwritable: ", strError());
    }
  }

  // Helper for workOnKillingCGroupTasks(), do NOT call directly.
  void killCGroupTasks(const std::string& subsystem) noexcept {
    // Uses /cgroup.procs instead of /tasks since it seems awkward and
    // unnecessary to signal each thread of a running process individually.
    std::unordered_set<uint64_t> pids;  // cgroup.procs need not be unique
    try {
      pids = folly::gen::byLine(
        folly::File(cgroupDir(subsystem) + "/cgroup.procs")
      ) | folly::gen::eachTo<uint64_t>()
        | folly::gen::as<decltype(pids)>();
    } catch (const std::exception& ex) {
      // Can happen if the system `release_agent` reaped the cgroup since
      // e.g. all processes quit after our last findNonEmptySubsystem().
      LOG(WARNING) << "Killing cgroup " << cgroupName_ << ": " << ex.what();
    }
    for (auto pid : pids) {
      // FATAL since none of the POSIX error conditions can occur, unless
      // we have a serious bug like signaling the wrong PID.
      PLOG_IF(FATAL, ::kill(pid, SIGKILL) == -1)
        << "Failed to kill " << pid << " from cgroup " << cgroupName_;
    }
  }

  /**
   * Only sends signals if the `freezer` subsystem is available, or if the
   * (dangerous) flag `killWithoutFreezer` is set.
   *
   * == How is `freezer` used? ==
   *
   * Signaling a cgroup is racy: between the time we read
   * `/cgroup.procs` and the time we send the signal, a process could exit,
   * and its PID could be recycled -- killing the wrong process.  This
   * mitigates the race by freezing the cgroup, sending SIGKILL, and thawing
   * the cgroup.  Since freezing the cgroup takes considerable time, the
   * work is spread over multiple calls to workOnKillingCGroupTasks() --
   * this is not a standalone synchronous operation.
   *
   * This gets called repeatedly, every curWaitMs_.  Each iteration focuses
   * on doing one state transition in this list:
   *  - THAWED => FREEZING or FROZEN
   *  - FREEZING => THAWED (if the freeze times out)
   *  - FROZEN => signaled & THAWED
   * Some of these take time, so it makes sense not to do them synchronously,
   * and instead to spread them out over multiple iterations.
   */
  void workOnKillingCGroupTasks(const std::string& nonempty_subsys) noexcept {
    // Signaling cgroups without `freezer` is racy and dangerous, see the
    // docstring in AsyncCGroupReaper.h.
    if (std::find(
      cgroupOpts_.subsystems.begin(), cgroupOpts_.subsystems.end(), "freezer"
    ) == cgroupOpts_.subsystems.end()) {
      // The client wants a racy, un-frozen kill, so go for it.
      if (cgroupOpts_.killWithoutFreezer) {
        killCGroupTasks(nonempty_subsys);
      } else {
        badFreezer(" is not enabled");
      }
      return;
    }

    auto freezer_state_path = cgroupDir("freezer") + "/freezer.state";
    std::string freezer_state;
    if (!folly::readFile(freezer_state_path.c_str(), freezer_state)) {
      badFreezer("'s state is unreadable: ", strError());
    } else if (freezer_state == "THAWED\n") {
      freezeWaitedMs_ = 0;
      freezerWrite(freezer_state_path, "FROZEN");
      // The next workOnKillingCGroupTasks() normally sees FREEZING or FROZEN.
    } else if (freezer_state == "FREEZING\n") {
      freezeWaitedMs_ += curWaitMs_;
      // Freezing can race with OOM-notifier logic, for example, see:
      //   https://issues.apache.org/jira/browse/MESOS-1689 & MESOS-1758
      //
      // Our simple mitigation is to time out a cgroup stuck in FREEZING,
      // re-thaw it, and try again.
      //
      // Future: Mesos decided to put its tasks into PID namespaces, which
      // lets the kernel kill the whole subtree.  I am not sure if this
      // approach is good for Bistro's low-overhead isolation, since PID
      // namespaces require a chroot & a separate /proc mount for the child
      // processes to have a good experience.  Relevant diff:
      // https://reviews.apache.org/r/25966/
      if (freezeWaitedMs_ > std::max(1, FLAGS_cgroup_freeze_timeout_ms)) {
        // freezeWaitedMs_ is not zeroed, so this can retry the next time.
        freezerWrite(freezer_state_path, "THAWED");
        // The next workOnKillingCGroupTasks() normally tries to freeze again.
      }
    } else if (freezer_state == "FROZEN\n") {
      // We are now as sure as possible that signaling these PIDs is safe.
      // We can only kill `freezer` tasks -- if another subsystem has other
      // PIDs, it's not safe to signal them anyhow.
      killCGroupTasks("freezer");
      // Non-D-state processes will receive the SIGKILL and exit shortly after.
      freezerWrite(freezer_state_path, "THAWED");
    } else {
      badFreezer("'s state is invalid (", freezer_state, ")");
    }
  }

  const cpp2::CGroupOptions cgroupOpts_;
  const std::string cgroupName_;

  // Start with a short timeout, in case the leaked processes die normally.
  const uint32_t minWaitMs_;
  // Intransigent processes that are not responding to SIGKILL are probably
  // in D state, so we exponentially back off to a longer delay.
  const uint32_t maxWaitMs_;

  // Used for exponential backoff, and for freeze timeouts.
  uint32_t curWaitMs_;
  uint64_t totalWaitMs_{0};  // Sum of all curWaitMs_

  // Let freeze attempts time out, since freezing can race with OOM-killer.
  uint64_t freezeWaitedMs_{0};

  folly::Promise<folly::Unit> emptyCGroups_;  // Fulfilled on self-destruction
};
}  // anonymous namespace

folly::Future<folly::Unit> asyncCGroupReaper(
    folly::EventBase* event_base,
    cpp2::CGroupOptions cgopts,
    std::string cgname,
    uint32_t min_wait_ms,  // Set to pollMs(TaskSubprocessOptions)
    // Only D-state tasks should end up retrying for a long time.  Note that
    // this should not be overly long, since freezing + signaling takes at
    // least two calls to workOnKillingCGroupTasks().
    uint32_t max_wait_ms) {
  folly::Future<folly::Unit> empty_cgroups;
  (new AsyncCGroupReaper(
    event_base, std::move(cgopts), std::move(cgname), min_wait_ms, max_wait_ms
  ))->initialize(&empty_cgroups);
  return empty_cgroups;
}

}}  // namespace facebook::bistro
