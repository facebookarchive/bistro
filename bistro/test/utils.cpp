/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/test/utils.h"

#include <boost/regex.hpp>
#include <iostream>

namespace facebook { namespace bistro {

// Reads incrementally from fd until the entirety what we have consumed on
// this run matches the given regex.  Caveat: this can easily consume more
// than you intended, preventing your next wait from matching.
void waitForRegexOnFd(folly::test::CaptureFD* fd, std::string regex) {
  std::string all;
  do {
    all += fd->readIncremental();  // So that ChunkCob fires incrementally
  } while (!boost::regex_match(all, boost::regex(std::move(regex))));
}

void printString(folly::StringPiece s) {
  if (!s.empty()) {
    std::cout << "stderr: " << s << std::flush;
  }
}

}}  // namespace facebook::bistro
