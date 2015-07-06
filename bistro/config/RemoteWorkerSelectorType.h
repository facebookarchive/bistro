#pragma once

#include <folly/FBString.h>

namespace facebook { namespace bistro {

enum class RemoteWorkerSelectorType {
  RoundRobin,
  Busiest
};

RemoteWorkerSelectorType getRemoteWorkerSelectorType(const folly::fbstring& s);

}}
