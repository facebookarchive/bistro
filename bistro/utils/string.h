#pragma once

#include <folly/Range.h>

namespace facebook { namespace bistro {

/**
 * If the last character is a newline, returns a string without it.
 */
inline folly::StringPiece stripNewline(folly::StringPiece s) {
  if (!s.empty() && s.back() == '\n') {
    s.pop_back();
  }
  return s;
}

}}
