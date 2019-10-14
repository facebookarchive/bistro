/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

namespace facebook { namespace bistro {

// Returns a string name for the signal, or "Unknown signal: number",
// Thread-safe, unlike strsignal().
std::string describeSignal(int sig);

}}  // namespace facebook::bistro
