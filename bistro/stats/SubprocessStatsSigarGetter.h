/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "bistro/bistro/stats/SubprocessStats.h"
#include <sigar.h>

namespace facebook { namespace bistro {

class SubprocessStatsSigarGetter : public SubprocessStatsGetter {
  friend class SubprocessStatsGetterFactory;
 public:
   explicit SubprocessStatsSigarGetter(pid_t processId = 0);
   ~SubprocessStatsSigarGetter();

   int getSystem(SubprocessSystem* available) override;
   void checkSystem() override;
   int getUsage(SubprocessUsage* usage) override;

 private:
  bool initialize();

  bool initRam();
  bool initCpus();
  bool initGpus();

 private:
  const sigar_pid_t processId_;
  sigar_t* sigar_{nullptr};
  SubprocessSystem installed_;
  uint64_t lastProcessCpuCycles_{0};
  uint64_t lastSystemCpuCycles_{0};
};

}}
