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
  // Sigar API doesn't provide the direct method to query gpu stats.
  // New approach is to run Nvidia utility "nvidia-smi" and get stats

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

  if (!initRam() ||
      !initCpus() ||
      !initGpus()) {
    return false;
  }

  LOG(INFO) << "System setup"
            << ", cpu cores: " << installed_.numberCpuCores
            << ", rss Mbytes: " << installed_.rssMBytes
            << ", gpu cores: " << installed_.numberGpuCores
            << ", gpu Mbytes: " << installed_.gpuMBytes;
  // request usage right away because some usage like cpu
  // can be calculated only as a difference in cpu time spent
  SubprocessUsage dummy;
  return 0 == getUsage(&dummy);
}

bool SubprocessStatsSigarGetter::initRam() {
  // get system installed RAM
  sigar_mem_t m;
  auto res = sigar_mem_get(sigar_, &m);
  if (res != SIGAR_OK) {
    LOG(ERROR) << "Failed to call sigar_mem_get, res: " << res;
    return false;
  }

  installed_.rssMBytes = m.ram; // ram in MB already
  return true;
}

bool SubprocessStatsSigarGetter::initCpus() {
  // get system cpu cores
  sigar_cpu_info_list_t cil;
  auto res = sigar_cpu_info_list_get(sigar_, &cil);
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
  return true;
}

bool SubprocessStatsSigarGetter::initGpus() {
  // get GPU index, memory, persistence mode & accounting mode
  // returns false only on format changes in nvidia-smi output
  // otherwise returns true even no GPUs are installed on the machine
  std::vector<std::string> stdOutLines, stdErrLines,
    queryFields{"memory.total", "persistence_mode", "accounting.mode"};
  auto retCode = subprocessOutputWithTimeout(
    {"/bin/sh", "-c", folly::to<std::string>(
     "nvidia-smi",
     ' ', "--query-gpu=", folly::join(',', queryFields),
     ' ', "--format=csv,noheader,nounits")
    }, &stdOutLines, &stdErrLines, kSubproccesTimeoutMs);

  uint64_t numberGpuCores = 0;
  if (retCode.exited() && retCode.exitStatus() == 0) {
    uint64_t gpuMBytesTotal = 0;
    // expected output
    /*
    0, 11519, Enabled, Enabled
    ...
    */
    // parse each line with the info per GPU.
    std::vector<std::string> stdOut, stdErr;
    for (const auto& line: stdOutLines) {
      std::vector<folly::StringPiece> parts;
      folly::split(", ", line, parts);
      if (parts.size() != queryFields.size()) {
        LOG(ERROR) << "Invalid number of fields"
                   << ", expected: " << queryFields.size()
                   << ", got: " << parts.size()
                   << ", input: " << line;
        return false;
      } else {
        removeTailNewLine(parts.back());
      }
      // conversion may throw
      try {
        gpuMBytesTotal += folly::to<int64_t>(parts[0]);
      } catch (const std::exception& e) {
        LOG(ERROR) << "Invalid field type format for nvidia-smi"
                   << ", reason: " << e.what()
                   << ", input: " << line;
        return false;
      }
      // count GPUs, we need only the number of GPUs
      // check if persistence mode and accounting mode are enabled
      if (parts[1] == "Enabled" && parts[2] == "Enabled") {
        ++numberGpuCores;
      } else {
        LOG(ERROR) << "Persistence mode and/or Accounting mode are disabled";
      }
    }
    // assign GPU cores
    installed_.numberGpuCores = numberGpuCores;

    if (numberGpuCores) {
      installed_.gpuMBytes = gpuMBytesTotal / numberGpuCores;
    }
  }
  return true;
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
       " --query-accounted-apps=", folly::join(',', queryFields),
       " --format=csv,noheader,nounits")
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

void SubprocessStatsSigarGetter::checkSystem() {
  // Sometimes, GPUs fail and disappear from nvidia-smi on a running system,
  // so monitor them continuously.
  initGpus();
}

int SubprocessStatsSigarGetter::getSystem(SubprocessSystem* available) {
  *available = installed_;
  return 0;
}

}}
