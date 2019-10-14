/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <limits>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

namespace detail {
inline void addValueToHash(cpp2::SetHash* h, int64_t v) {
  h->xorAll = h->xorAll ^ v;
  h->addAll = static_cast<int64_t>(
      static_cast<uint64_t>(h->addAll) + static_cast<uint64_t>(v));
}
inline void removeValueFromHash(cpp2::SetHash* h, int64_t v) {
  h->xorAll = h->xorAll ^ v;
  h->addAll = static_cast<int64_t>(
      static_cast<uint64_t>(h->addAll) - static_cast<uint64_t>(v));
}
}

inline void addWorkerIDToHash(
    cpp2::WorkerSetHash* s,
    cpp2::BistroInstanceID id) {
  ++s->numWorkers;
  detail::addValueToHash(&s->startTime, id.startTime);
  detail::addValueToHash(&s->rand, id.rand);
}

inline void removeWorkerIDFromHash(
    cpp2::WorkerSetHash* s,
    cpp2::BistroInstanceID id) {
  CHECK_LT(0, s->numWorkers);
  --s->numWorkers;
  detail::removeValueFromHash(&s->startTime, id.startTime);
  detail::removeValueFromHash(&s->rand, id.rand);
}

/**
 * Less-than comparator for WorkerSetID history versions, assuming that a
 * and b are increment-only and can never be separated by more than 2^63 - 1
 * steps (that would require some extremely rapid worker turnover and a very
 * generous "worker lost" timeout).  Assumes two's complement.  Future: add
 * a static assert in case we're not compiled on a two's complement box.
 */
struct WorkerSetIDEarlierThan {
  // The C++ standard does not define how unsigned -> signed conversion must
  // proceed, so use 2's complement just like this logic needs.
  int64_t makeSigned(uint64_t v) const {
    return (v < 0x8000000000000000) ? v : (
      std::numeric_limits<int64_t>::min() + (v - 0x8000000000000000)
    );
  }
  // Take unsigned arguments because signed => unsigned implicit conversion
  // is well-defined by C++, but the opposite direction isn't... and we need
  // unsigned ints below for the subtraction anyway.
  //   http://en.cppreference.com/w/cpp/language/implicit_conversion
  bool operator()(uint64_t a, uint64_t b) const {
    // The C++ fails to specify signed overflow behavior, and GCC + ASAN +
    // optimizations actually seems to break some of our unit tests, so do
    // an unsigned subtraction and then coerce according to 2's complement.
    if (makeSigned(a - b) >= 0) {
      return false;
    } else if (makeSigned(b - a) > 0) {
      return true;
    }
    LOG(FATAL) << "Versions differ by 2^63: a = " << a << ", b = " << b;
    return false;  // not reached but GCC isn't smart enough
  }
};

}}  // namespace facebook::bistro
