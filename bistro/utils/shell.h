/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

namespace facebook { namespace bistro {

/**
 * If cmd is a vector of the sort you'd pass to folly::Subprocess, this is
 * meant to be an easy way to log the vector, such that you can copy-paste
 * the log output, and run the command that was being executed.
 *
 * Do NOT use this to escape user input. Although this function is believed
 * to be correct, there was no attempt to make a test suite proving its
 * security.  It is intended to produce helpful & properly escaped debug
 * output, not to thwart an adversary.
 *
 * If you need a secure function, find a known-good one, or beef up the test
 * suite of this one.
 */
const std::string escapeShellArgsInsecure(const std::vector<std::string>& cmd);

}}
