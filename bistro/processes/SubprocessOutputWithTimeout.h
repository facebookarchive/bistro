/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/processes/AsyncSubprocess.h"

namespace facebook { namespace bistro {

/**
 * Waits (blocking call) for subprocess completion, returns subprocess return
 * code, stdout and stderr lines. Newlines are preserved, lines longer than 2048
 * characters are broken up into adjacent "lines" with no trailing newlines
 * Caller provides command line (array of strings) and timeout.
 * Important!!!
 * If subprocess opens a next generation of subprocesses with the inherited
 * stderr/stdout FDs and some level of independence, i.e detach themselves from
 * the process group and keep the stderr/stdout FDs open then timeout won't
 * work, because function will be waiting for stderr/stdout FDs get closed.
 * In such case function would hang up till life time of sub-subprocesses.
 */
folly::ProcessReturnCode subprocessOutputWithTimeout(
    const std::vector<std::string>& cmd,
    std::vector<std::string>* stdOutLines,
    std::vector<std::string>* stdErrLines,
    uint32_t timeoutMs,
    uint32_t pollMs = 10);

}}  // namespace facebook::bistro
