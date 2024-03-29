/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

namespace facebook { namespace bistro {

enum class RemoteWorkerSelectorType {
  RoundRobin,
  Busiest
};

RemoteWorkerSelectorType getRemoteWorkerSelectorType(const std::string& s);

}}
