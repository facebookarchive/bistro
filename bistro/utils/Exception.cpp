/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

std::string strError() {
  char buf[512];
  // The const char* is important -- this is written for the GNU version
  // of strerror_r (although a preprocessor test could support XSI too).
  const char* error = strerror_r(errno, buf, sizeof(buf));
  PCHECK(error != nullptr) << "strerror_r failed";
  return std::string(error);
}

}}  // namespace facebook::bistro
