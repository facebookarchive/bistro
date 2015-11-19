/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "bistro/bistro/stats/SubprocessStatsSigarGetter.h"
#include "bistro/bistro/processes/SubprocessOutputWithTimeout.h"
#include <unistd.h>

namespace facebook { namespace bistro {

namespace {
  // Sigar API doesn't provide the direct method to query network stats.
  // Way around is to get list of all connections, for each connection get port,
  // for each port get associated pid, compare with subprocess pid, and if there
  // is a match=, request stats for that port. This is VERY slow

  // Another approach to read stats from subprocess file
  // cat /proc/<PID>/net/netstat | grep IpExt
  // IpExt: InNoRoutes InTruncatedPkts InMcastPkts OutMcastPkts InBcastPkts
  //        OutBcastPkts InOctets OutOctets InMcastOctets OutMcastOctets
  //        InBcastOctets OutBcastOctets InCsumErrors
  // IpExt: 3224 0 42679 42679 96946
  //        3676 79239769367 79188721747 7089645 7089645
  //        48172433 611797 0
  // So we can get InOctets & OutOctets as totally transmitted & received bytes
  // and calculate the difference

  // For disk IO the following command line (requires root privileges)
  // sudo iotop -b -n 1 -p <PID>
  // Total DISK READ:       0.00 B/s | Total DISK WRITE:       0.00 B/s
  // TID     PRIO  USER  DISK READ  DISK WRITE  SWAPIN      IO    COMMAND
  // 1827763 be/4  root  0.00 B/s   0.00 B/s    0.00 %      0.00  ...

  inline void removeTailNewLine(folly::StringPiece& str) {
    if (!str.empty() && str.back() == '\n') {
      str.subtract(1);
    }
  }
  const uint32_t kSubproccesTimeoutMs = 1000;
}


SubprocessStatsSigarGetter::SubprocessStatsSigarGetter(pid_t processId)
  : processId_(processId ? processId : getpid()) {
}

SubprocessStatsSigarGetter::~SubprocessStatsSigarGetter() {
  if (sigar_) {
    sigar_close(sigar_);
  }
}

bool SubprocessStatsSigarGetter::initialize() {
  auto res = sigar_open(&sigar_);
  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_open, res: " << res;
    return false;
  }

  // get system cpu cores
  sigar_cpu_info_list_t cil;
  res = sigar_cpu_info_list_get(sigar_, &cil);
  const auto totalNumberCpuCores = cil.number;
  sigar_cpu_info_list_destroy(sigar_, &cil);

  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_cpu_info_list_get, res: " << res;
    return false;
  } if (totalNumberCpuCores < 1) {
    LOG(ERROR) << "Invalid number of cpu cores: " << totalNumberCpuCores;
    return false;
  }

  installed_.numberCpuCores = totalNumberCpuCores;

  // get system installed RAM
  sigar_mem_t m;
  res = sigar_mem_get(sigar_, &m);
  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_mem_get, res: " << res;
    return false;
  }

  installed_.rssMBytes = m.ram; // ram in MB already

  // get GPU index, memory, persistence mode & accounting mode
  std::vector<std::string> stdOutLines, stdErrLines,
    queryFields{"index", "memory.total", "persistence_mode", "accounting.mode"};
  auto retCode = subprocessOutputWithTimeout(
    {"/bin/sh", "-c", folly::to<std::string>(
     "nvidia-smi",
     ' ', "--query-gpu=", folly::join(',', queryFields),
     ' ', "--format=csv,noheader,nounits")
    }, &stdOutLines, &stdErrLines, kSubproccesTimeoutMs);

  installed_.numberGpuCores = 0;
  if (retCode.exited() && retCode.exitStatus() == 0) {
    uint64_t gpuMBytesTotal = 0;
    // expected output
    /*
    0, 11519, Enabled, Disabled
    1, 11519, Enabled, Disabled
    2, 11519, Enabled, Disabled
    */
    // just get the number of lines and query each gpu separately.
    std::vector<std::string> stdOut, stdErr;
    for (const auto& line: stdOutLines) {
      std::vector<folly::StringPiece> parts;
      folly::split(", ", line, parts);
      if (parts.size() != queryFields.size()) {
        LOG(FATAL) << "Invalid number of fields"
                   << ", expected: " << queryFields.size()
                   << ", got: " << parts.size()
                   << ", input: " << line;
      } else {
        removeTailNewLine(parts.back());
      }
      // conversion may throw
      int64_t idx = 0;
      try {
        idx = folly::to<int64_t>(parts[0]);
        gpuMBytesTotal += folly::to<int64_t>(parts[1]);
      } catch (const std::exception& e) {
        LOG(FATAL) << "Invalid field type format for nvidia-smi"
                   << ", reason: " << e.what()
                   << ", input: " << line;
      }
      // count GPUs, we need only the number of GPUs
      ++installed_.numberGpuCores;
      if (parts[2] != "Enabled") {
        LOG(INFO) << "Enable persistence mode for gpu #" << idx;
        // enable persistence mode
        stdOut.clear();
        stdErr.clear();
        retCode = subprocessOutputWithTimeout(
          {"/bin/sh", "-c", folly::to<std::string>(
           "sudo nvidia-smi", ' ', "-i", ' ', idx,
           ' ', "-pm ENABLED")
          }, &stdOut, &stdErr, kSubproccesTimeoutMs);
        if (retCode.exited() && retCode.exitStatus() == 0) {
          LOG(INFO) << "Persistence mode is enabled.";
        } else {
          LOG(ERROR) << "Cannot enable persistence mode"
                     << ", reason:" << folly::join(' ', stdErr);
        }
      }
      if (parts[3] != "Enabled") {
        // enable accounting mode
        LOG(INFO) << "Enable accounting for gpu #" << idx;
        stdOut.clear();
        stdErr.clear();
        retCode = subprocessOutputWithTimeout(
          {"/bin/sh", "-c", folly::to<std::string>(
           "sudo nvidia-smi", ' ', "-i", ' ', idx,
           ' ', "-am ENABLED")
          }, &stdOut, &stdErr, kSubproccesTimeoutMs);
        if (retCode.exited() && retCode.exitStatus() == 0) {
          LOG(INFO) << "Accounting mode is enabled.";
        } else {
          LOG(ERROR) << "Cannot enable accounting mode"
                     << ", reason:" << folly::join(' ', stdErr);
        }
      }
    }
    if (installed_.numberGpuCores) {
      installed_.gpuMBytes = gpuMBytesTotal / installed_.numberGpuCores;
    }
  } else {
    LOG(INFO) << "Cannot query gpu properties for available gpus"
              << ", reason: " << folly::join(' ', stdErrLines);
  }

  LOG(INFO) << "System setup"
            << ", cpu cores: " << installed_.numberCpuCores
            << ", rss Mbytes: " << installed_.rssMBytes
            << ", gpu cpores: " << installed_.numberGpuCores
            << ", gpu Mbytes: " << installed_.gpuMBytes;
  // request usage right away because some usage like cpu
  // can be calculated only as a difference in cpu time spent
  SubprocessUsage dummy;
  return 0 == getUsage(&dummy);
}

int SubprocessStatsSigarGetter::getUsage(SubprocessUsage* usage) {
  // memory
  sigar_proc_mem_t procmem;
  auto res = sigar_proc_mem_get(sigar_, processId_, &procmem);
  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_proc_mem_get, res: " << res;
    return res;
  }

  usage->rssMBytes = procmem.resident / 1024 / 1024; // convert to MB

  // cpu
  // process
  sigar_proc_cpu_t proccpu;
  res = sigar_proc_cpu_get(sigar_, processId_, &proccpu);
  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_proc_cpu_get, res: " << res;
    return res;
  }

  const auto processTotal = proccpu.total;

  // cpu
  // system
  sigar_cpu_t c;
  res = sigar_cpu_get(sigar_, &c);
  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_cpu_get, res: " << res;
    return res;
  }

  const auto systemTotal = c.total;

  uint64_t diffProcess = 0, diffSystem = 0;
  if (lastSystemCpuCycles_) { // can get a difference
    // get diffs
    diffProcess = processTotal - lastProcessCpuCycles_;
    diffSystem = systemTotal - lastSystemCpuCycles_;
  }

  // calculate cpu usage
  usage->numberCpuCores = diffSystem
    ? installed_.numberCpuCores * diffProcess / diffSystem
    : 0;

  // remember last cycles
  lastProcessCpuCycles_ = processTotal;
  lastSystemCpuCycles_ = systemTotal;

  // - get gpu stats, if any installed
  double gpuCoresUsagePercentage = 0., gpuMemoryUsagePercentage = 0.;
  if (installed_.numberGpuCores) {
    std::vector<std::string> stdOutLines, stdErrLines,
      queryFields{"pid", "gpu_util", "mem_util"};
    folly::ProcessReturnCode retCode = subprocessOutputWithTimeout(
      {"/bin/sh", "-c", folly::to<std::string>(
       "nvidia-smi",
       ' ', "--query-accounted-apps=", folly::join(',', queryFields),
       ' ', "--format=csv,noheader,nounits")
      },
      &stdOutLines, &stdErrLines, kSubproccesTimeoutMs);

    if (retCode.exited() && retCode.exitStatus() == 0) {
      // parse expected output
      for (const auto& outLine: stdOutLines) {
        // split, get pid, and carry on
        std::vector<folly::StringPiece> parts;
        folly::split(", ", outLine, parts);
        if (parts.size() != queryFields.size()) {
          LOG(FATAL) << "Invalid number of fields"
                     << ", expected: " << queryFields.size()
                     << ", got: " << parts.size()
                     << ", input: " << outLine;
        } else {
          removeTailNewLine(parts.back());
        }
        // conversion may throw
        try {
          const int64_t pid = folly::to<int64_t>(parts[0]);
          if (pid != processId_) {
            continue;
          }
          // get core & core memory usage
          gpuCoresUsagePercentage += folly::to<double>(parts[1]);
          gpuMemoryUsagePercentage += folly::to<double>(parts[2]);
        } catch (const std::exception& e) {
          LOG(FATAL) << "Invalid field type format for usage"
                     << ", reason: " << e.what()
                     << ", input: " << outLine;
        }
      }
    } else {
      LOG(FATAL) << "Cannot get gpu usage"
                 << ", reason: " << folly::join(' ', stdErrLines);
    }
    // convert percentage usage back to unit usage
    usage->numberGpuCores = gpuCoresUsagePercentage / 100.;
    usage->gpuMBytes = gpuMemoryUsagePercentage / 100.;
  }

  return 0;
}

int SubprocessStatsSigarGetter::getSystem(SubprocessSystem* available) {
  *available = installed_;
  return 0;
}

}}
