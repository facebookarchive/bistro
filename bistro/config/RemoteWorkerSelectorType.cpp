/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/RemoteWorkerSelectorType.h"

#include <unordered_map>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

namespace {
  std::unordered_map<std::string, RemoteWorkerSelectorType> type_map = {
    { "roundrobin", RemoteWorkerSelectorType::RoundRobin },
    { "busiest", RemoteWorkerSelectorType::Busiest },
  };
}

RemoteWorkerSelectorType getRemoteWorkerSelectorType(const std::string& s) {
  auto it = type_map.find(s);
  if (it == type_map.end()) {
    throw BistroException("Unknown remote_worker_selector type: ", s);
  }
  return it->second;
}

}}
