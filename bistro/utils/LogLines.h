/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <limits>

namespace facebook { namespace bistro {

// TODO: Get rid of this in favor of the Thrift struct as soon as we figure
// out our Thrift story? (#3885590)
struct LogLine {
  // Marks LogLines that are not authentic log lines, but which instead store
  // some kind of error or warning about the log line fetching process.
  enum { kNotALineID = -1 };

  // TODO(agoder): Can haz zero-copy construction?
  LogLine(
    std::string job_id,
    std::string node_id,
    int32_t time,
    std::string line,
    int64_t line_id
  ) : jobID(job_id),
      nodeID(node_id),
      time(time),
      line(line),
      lineID(line_id) {}

  std::string jobID;
  std::string nodeID;
  int32_t time;
  std::string line;
  int64_t lineID;

  static int64_t makeLineID(int32_t time, uint32_t count) {
    return ((int64_t)time << 32) | count;
  }

  static int64_t lineIDFromTime(int32_t time, bool is_ascending) {
    return LogLine::makeLineID(
      time,
      is_ascending
        ? std::numeric_limits<uint32_t>::min()
        : std::numeric_limits<uint32_t>::max()
    );
  }

  static int32_t timeFromLineID(int64_t line_id) {
    return line_id >> 32;
  }

};

struct LogLines {
  // Can contain "error" lines whose lineID == kNotALineID
  std::vector<LogLine> lines;
  int64_t nextLineID;  // If no more lines, equals kNotALineID
};

}}
