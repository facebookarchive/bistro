/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/signal.h"

#include <folly/Conv.h>
#define _GNU_SOURCE 1
#include <signal.h>
#include <string.h>

namespace facebook { namespace bistro {

std::string describeSignal(int sig) {
#if __cplusplus >= 202002L
  // sigdescr_np is thread-safe, unlike strsignal().
  auto desc = sigdescr_np(sig);
  return desc ? desc : folly::to<std::string>("Unknown signal: ", sig);
#else
  // FIXME TODO: remove this else clause, once platform009 is gone.
  // sys_siglist should be thread-safe, unlike strsignal().
  if (sig >= 0 && sig < sizeof(::sys_siglist)/sizeof(*::sys_siglist)) {
    return ::sys_siglist[sig];
  }
#endif
  return folly::to<std::string>("Unknown signal: ", sig);
}

}}  // namespace facebook::bistro
