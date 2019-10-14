/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/Exception.h"

#include <glog/logging.h>

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
