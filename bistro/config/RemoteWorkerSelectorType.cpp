#include "bistro/bistro/config/RemoteWorkerSelectorType.h"

#include <unordered_map>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

namespace {
  std::unordered_map<folly::fbstring, RemoteWorkerSelectorType> type_map = {
    { "roundrobin", RemoteWorkerSelectorType::RoundRobin },
    { "busiest", RemoteWorkerSelectorType::Busiest },
  };
}

RemoteWorkerSelectorType getRemoteWorkerSelectorType(
    const folly::fbstring& s) {

  auto it = type_map.find(s);
  if (it == type_map.end()) {
    throw BistroException("Unknown remote_worker_selector type: ", s);
  }
  return it->second;
}

}}
