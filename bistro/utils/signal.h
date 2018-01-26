/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <string>

namespace facebook { namespace bistro {

// Returns a string name for the signal, or "Unknown signal: number",
// Thread-safe, unlike strsignal().
std::string describeSignal(int sig);

}}  // namespace facebook::bistro
