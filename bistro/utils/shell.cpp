/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/shell.h"

#include <boost/algorithm/string.hpp>
#include <folly/String.h>

namespace facebook { namespace bistro {

const std::string escapeShellArgsInsecure(
    const std::vector<std::string>& cmd) {

  std::vector<std::string> escaped_cmd;
  for (const auto& arg : cmd) {
    escaped_cmd.push_back(arg);
    boost::replace_all(escaped_cmd.back(), "'", "'\\''");
  }
  return "'" + folly::join("' '", escaped_cmd) + "'";
}

}}
