/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/processes/CGroupSetup.h"

#include <boost/filesystem.hpp>
#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/String.h>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

//
// DANGER DANGER DANGER
//
// This runs in the child process after vfork, and can wreak HAVOC.
// Some guidelines if you must change this:
//
// - Read http://ewontfix.com/7/
// - Be quick -- the parent thread is blocked until you exit.
// - Avoid mutating any data belonging to the parent.
// - Avoid accessing data that can be mutated by running parent threads,
//   e.g. std::string::c_str() is not certain to be safe.
// - Avoid interacting with non-POD data that came from the parent.
// - Avoid any libraries that may internally reference non-POD state.
// - Especially beware parent mutexes, e.g. LOG() uses a global mutex.
// - Avoid invoking the parent's destructors (you can accidentally
//   delete files, terminate network connections, etc).
//
// DANGER DANGER DANGER
//
// In this function, we pray that nobody adds clowny global-state behavior
// to the folly functions used below.
//
int AddChildToCGroups::operator()() {
  // Add the child to CGroups **before** it has a chance to start its own
  // subprocesses.
  //
  // We are going to try to add the child to **all** cgroups, even if some
  // hit an error.  Why keep going in the face of failure?  A major reason
  // for failure would be that some of the CGroups are not set up correctly.
  // If that's the case, we still want to add this about-to-die child to any
  // valid CGroups to trigger their "notify_on_release" to avoid leaks.  If
  // they don't use "notify_on_release", no harm done either.
  int last_errno = 0;  // The last error, or 0 if none happened.
  for (const auto& cgpath : cgroupProcsPaths_) {
    // O_APPEND slightly helps the hapless user who uses a real file path.
    //
    // c_str() is only safe because cgroupProcsPaths_ is const, and thus
    // safe from a different thread of the parent modifying it.
    int fd = folly::openNoInt(cgpath.c_str(), O_WRONLY | O_APPEND);
    if (fd == -1) {
      last_errno = errno;
    // Add my PID, using "0" avoids getpid() call.
    } else if (folly::writeFull(fd, "0", 1) != 1) {
      last_errno = errno;
      folly::closeNoInt(fd);
    // The write only succeeds when close() does.
    } else if (folly::closeNoInt(fd) == -1) {
      last_errno = errno;
    }
  }
  return last_errno;
}

namespace {
template <class... Args>
std::string makeUnixError(Args&&... args) noexcept {
  return folly::to<std::string>(std::forward<Args>(args)..., ": ", strError());
}

bool assertReadBackSameValue(int64_t written, int64_t read_back) {
  return written == read_back;
}

// Control files exist from the get-go in real cgroups -- we want to require
// this, since it serves as a sanity check against cgroups not being mounted
// correctly.  This behavior is hard to simulate in unit tests (since we're
// just making directories), so we **only** enable O_CREAT in tests.
// O_APPEND isn't necessary, but in case of bad paths being passed in, it's
// less destructive to data on disk.
int cgFileFlags(cpp2::CGroupOptions opts) {
  return O_WRONLY | O_APPEND | (opts.unitTestCreateFiles ? O_CREAT : 0);
}

// Write a value to a cgroups file, and read it back. Use check_read_back_fn
// to compare the read-back value with the written one. Returns true
// if all operations succeeded, false otherwise.
//
// Why read back? E.g. memory.usage_in_bytes may contain a different value
// than the one written (per cgroups/memory.txt in the kernel docs).
bool writeToCGroupFile(
    std::vector<std::string>* errors,
    const boost::filesystem::path& dir,
    folly::StringPiece basename,
    const int64_t value,
    int cgfile_flags,
    std::function<bool(int64_t, int64_t)> check_read_back_fn
      = assertReadBackSameValue) {

  auto filename = (dir / basename.str()).native();

  if (!folly::writeFile(
    folly::to<std::string>(value), filename.c_str(), cgfile_flags
  )) {
    errors->emplace_back(makeUnixError("Writing ", value, " to",  filename));
    return false;
  }

  // Read back the value, since e.g. memory.limit_in_bytes requires this.
  std::string read_back_str;
  if (!folly::readFile(filename.c_str(), read_back_str)) {
    errors->emplace_back(makeUnixError(
      "Could not read back ", value, " from ", filename
    ));
    return false;
  }

  int64_t read_back;
  try {
    read_back = folly::to<int64_t>(read_back_str);
  } catch (const std::exception& ex) {
    errors->emplace_back(makeUnixError(
      "Read back non-int ", read_back_str, " from ", filename, ", not ", value
    ));
    return false;
  }

  // Did the value we read back match our expectations? E.g. for
  // memory.limit_in_bytes the kernel may alter the requested limit.
  if (!check_read_back_fn(value, read_back)) {
    errors->emplace_back(makeUnixError(
      "Wrote ", value, " to ", filename, ", but read back ", read_back
    ));
    return false;
  }

  return true;
}
}  // anonymous namespace

std::vector<std::string> cgroupSetup(
    const std::string& cgname,
    const cpp2::CGroupOptions& cg) {

  // Make the cgroup dirs. If any fail, remove all created ones, then throw.
  std::vector<std::string> dirs;  // Will be removed on error.
  std::vector<std::string> procs_paths;  // The return value.
  boost::system::error_code ec;  // Reused, error messages go into `errors`.
  std::vector<std::string> errors;
  for (const auto& subsystem : cg.subsystems) {
    auto slice_dir = boost::filesystem::path(cg.root) / subsystem / cg.slice;
    // The root & slice have to exist, otherwise the system is misconfigured.
    if (!boost::filesystem::is_directory(slice_dir, ec) || ec) {
      errors.emplace_back(folly::to<std::string>(
        "CGroup root/subsystem/slice must be a directory: ",
        slice_dir.native(), (ec ? ": " + ec.message() : "")
      ));
      continue;
    }
    // Make the cgroup. On error, we will reap all new cgroups after the loop.
    dirs.emplace_back((slice_dir / cgname).native());
    if (!boost::filesystem::create_directories(dirs.back(), ec)) {
      if (ec) {
        errors.emplace_back(folly::to<std::string>(
          "Creating ", dirs.back(), ": ", ec.message()
        ));
      } else {
        errors.emplace_back(folly::to<std::string>(
          "CGroup ", dirs.back(), " already exists"
        ));
      }
      dirs.pop_back();  // No directory to remove at cleanup time.
      continue;
    }
    // Check if "/cgroup.procs" is writable in our just-made cgroup.
    procs_paths.emplace_back(folly::to<std::string>(
      dirs.back(), "/", kCGroupProcs
    ));
    int fd = folly::openNoInt(procs_paths.back().c_str(), cgFileFlags(cg));
    if (fd == -1 || folly::closeNoInt(fd) == -1) {
      errors.emplace_back(makeUnixError("Cannot write ", procs_paths.back()));
      continue;
    }
    // Let the cgroup be auto-removed when empty (if "release_agent" is set).
    //
    // NB We could instead have set this on the parent cgroup, but that
    // seems more likely to cause problems down the line, if e.g. we decide
    // to reuse cgroups for perf reasons.
    if (!writeToCGroupFile(
      &errors, dirs.back(), kNotifyOnRelease, 1, cgFileFlags(cg)
    )) {
      continue;
    }
    // Limit CPU usage.
    if (subsystem == kCPU && cg.cpuShares && !writeToCGroupFile(
      &errors, dirs.back(), kCPUShares, cg.cpuShares, cgFileFlags(cg)
    )) {
      continue;
    }
    // Limit RAM usage.
    //
    // NB If you ever decide to use the OOM notifier instead of a hard limit,
    // keep in mind that this can race with the freezer. Details here:
    // https://issues.apache.org/jira/browse/MESOS-1689 & MESOS-1758
    if (subsystem == kMemory && cg.memoryLimitInBytes
        && !writeToCGroupFile(
          &errors, dirs.back(), kMemoryLimitInBytes, cg.memoryLimitInBytes,
          cgFileFlags(cg), [&] (int64_t written, int64_t read_back) {
            // We don't mind if the kernel rounds up by up to 1MB.
            return read_back >= written && (written - read_back <= (1 << 20));
          }
        )) {
      continue;
    }
  }

  // On error, try to remove created directories to avoid leaking cgroups.
  if (!errors.empty()) {
    for (const auto& path : dirs) {
      boost::filesystem::remove(path, ec);
      if (ec) {
        errors.emplace_back(folly::to<std::string>(
          "Removing ", path, ": ", ec.message()
        ));
      }
    }
    throw BistroException(
      "Failed to make cgroup directories: ", folly::join("; ", errors)
    );
  }

  return procs_paths;
}

}}  // namespace facebook::bistro
