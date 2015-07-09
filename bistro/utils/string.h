/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
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
