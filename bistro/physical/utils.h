/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <folly/Conv.h>
#include <folly/Demangle.h>
#include <folly/Exception.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <set>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

// Splits a line by a delimiter and removes the trailing newline.  Throws if
// the number of parts does not match the expected count.
template<typename T>
std::vector<folly::StringPiece> checkedSplitLine(
    const std::string& delimiter,
    const T& line,
    size_t expected_parts) {
  std::vector<folly::StringPiece> out;
  folly::split(delimiter, line, out);
  if (out.size() != expected_parts) {
    throw BistroException(
      "Expected ", expected_parts, "fields , but got ", out.size(),
      " in line: ", line
    );
  }
  if (!out.empty()) {
    out.back().removeSuffix('\n');
  }
  return out;
}

template<typename T>
T valueFromFile(const boost::filesystem::path& filename) {
  std::string line;
  if (!folly::readFile(filename.c_str(), line)) {
    folly::throwSystemError("Could not read from ", filename.native());
  }
  folly::StringPiece s(line);
  s.removeSuffix('\n');
  try {
    return folly::to<T>(s);
  } catch (const std::exception& ex) {
    throw BistroException(
      "Converting value ", s, " from ", filename.native(), " to ",
      folly::demangle(typeid(T)), ": ", ex.what()
    );
  }
}

// Parses a set of uint64_t values from a string containing a
// comma-separated list of ranges (X-Y) and single values (Y).
std::set<uint64_t> parseIntSet(const std::string&);

// Returns an empty vector when nvidia-smi monitoring is disabled, or the
// `nvidia-smi` output lines (with newlines) otherwise.
std::vector<std::string> queryNvidiaSmi(
  const char* query_type,  // e.g. "query-gpu"
  std::vector<std::string> query_fields,  // e.g. {"gpu_bus_id"}
  uint32_t timeout_ms
);

}}  // namespace facebook::bistro
