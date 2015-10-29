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
  : processId_(processId ? processId : getpid())
  , sigar_(nullptr) {
}

SubprocessStatsSigarGetter::~SubprocessStatsSigarGetter() {
  if (sigar_) {
    sigar_close(sigar_);
  }
}

int SubprocessStatsSigarGetter::initialize() {
  auto res = sigar_open(&sigar_);
  if (res != SIGAR_OK) {
    return res;
  }
  // request usage right away because some usage like cpu
  // can be calculated only as a difference in cpu time spent
  SubprocessUsage dummy;
  return getUsage(&dummy);
}

int SubprocessStatsSigarGetter::getUsage(SubprocessUsage* usage) {
  // memory
  sigar_proc_mem_t procmem;
  auto res = sigar_proc_mem_get(sigar_, processId_, &procmem);
  if (res != SIGAR_OK) {
    return res;
  }
  usage->rssBytes = procmem.resident;
  usage->totalBytes = procmem.size;
  // cpu
  sigar_proc_cpu_t proccpu;
  res = sigar_proc_cpu_get(sigar_, processId_, &proccpu);
  if (res != SIGAR_OK) {
    return res;
  }
  usage->userCpu = proccpu.user;
  usage->sysCpu = proccpu.sys;
  usage->totalCpu = proccpu.total;
  return 0;
}

}}
