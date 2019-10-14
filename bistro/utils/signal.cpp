/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
