/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/signal.h"

#include <folly/Conv.h>
#include <signal.h>

namespace facebook { namespace bistro {

std::string describeSignal(int sig) {
  // sys_siglist should be thread-safe, unlike strsignal().
  if (sig >= 0 && sig < sizeof(::sys_siglist)/sizeof(*::sys_siglist)) {
    return ::sys_siglist[sig];
  }
  return folly::to<std::string>("Unknown signal: ", sig);
}

}}  // namespace facebook::bistro
