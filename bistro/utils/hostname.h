#pragma once

#include <boost/algorithm/string/predicate.hpp>
#include <string>

namespace facebook { namespace bistro {

/**
 * Returns a fully qualified hostname for the current machine, or an empty
 * string on error.  This is intended to be canonical and unique.
 * Assumption: If a hostname is transfered to a new machine, all processes
 * on the old machine have died.
 *
 * See the comment on struct BistroWorker's hostname for more context.
 */
std::string getLocalHostName();

template<class Container>
bool startsWithAny(const std::string& s, const Container& c) {
  for (const auto& prefix : c) {
    if (boost::starts_with(s, prefix)) {
      return true;
    }
  }
  return false;
}

}}
