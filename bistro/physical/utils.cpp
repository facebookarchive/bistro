/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/physical/utils.h"

#include <folly/gen/File.h>

#include "bistro/bistro/processes/SubprocessOutputWithTimeout.h"

DEFINE_string(
  nvidia_smi, "",
  "By default, Bistro will not monitor GPU resources. Set this argument to "
  "point at the binary, e.g. /usr/bin/nvidia-smi, to enable GPU monitoring."
);

namespace facebook { namespace bistro {

std::set<uint64_t> parseIntSet(const std::string& s) {
  std::set<uint64_t> nums;
  std::vector<folly::StringPiece> parts;
  folly::split(",", s, parts);  // split 0-1,4 by comma
  for (const auto& part : parts) {
    std::vector<folly::StringPiece> range;
    folly::split("-", part, range);  // split 0-1 by dash
    try {
      if (range.size() == 1) {
        nums.insert(folly::to<uint64_t>(range[0]));
      } else if (range.size() == 2) {
        auto from = folly::to<uint64_t>(range[0]);
        auto to = folly::to<uint64_t>(range[1]);
        if (to < from) {
          throw BistroException("Bad range ", part, " in integer set ", s);
        }
        for (; from <= to; ++from) {
          nums.insert(from);
        }
      } else {
        throw BistroException("Bad integer set part '", part, "', from ", s);
      }
    } catch (const std::exception& ex) {
      throw BistroException(
        "Parsing integer set part ", s, " from ", s, ": ", ex.what()
      );
    }
  }
  return nums;
}

std::vector<std::string> queryNvidiaSmi(
    const char* query,
    std::vector<std::string> query_fields,
    uint32_t timeout_ms) {
  if (FLAGS_nvidia_smi.empty()) {
    return {};  // GPU monitoring disabled
  }
  std::vector<std::string> out, err;
  std::vector<std::string> nvidia_smi_cmd{
    FLAGS_nvidia_smi,
    "--format=csv,noheader,nounits",
    folly::to<std::string>("--", query, "=", folly::join(',', query_fields))
  };
  auto r = subprocessOutputWithTimeout(nvidia_smi_cmd, &out, &err, timeout_ms);
  if (!r.exited() || r.exitStatus() != 0) {
    throw BistroException(
      "nvidia-smi query '", folly::join("' '", nvidia_smi_cmd), "' failed "
      "(", r.str(), "): ", folly::join('\n', err)
    );
  }
  return out;
}

}}  // namespace facebook::bistro
