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
#include <unistd.h>

namespace facebook { namespace bistro {

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

  LOG(INFO) << "System setup"
            << ", cpu cores: " << installed_.numberCpuCores
            << ", rss Mbytes: " << installed_.rssMBytes;
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
  return 0;
}

int SubprocessStatsSigarGetter::getSystem(SubprocessSystem* available) {
  *available = installed_;
  return 0;
}

}}
