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

#include <atomic>
#include <boost/filesystem/path.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "bistro/bistro/utils/BackgroundThreadMixin.h"
#include "bistro/bistro/utils/EnumHash.h"
#include "bistro/bistro/utils/ProcessRunner.h"  // for enum class LogTable
#include <folly/Range.h>
#include <folly/Synchronized.h>

namespace facebook { namespace bistro {

namespace sqlite {
  class Database;
  class Statement;
}

class LogLines;

class LogWriter : BackgroundThreadMixin {

public:
  explicit LogWriter(const boost::filesystem::path& db_file);
  ~LogWriter() override;

  void write(
    LogTable table,
    const std::string& job,
    const std::string& node,
    folly::StringPiece line
  );

  LogLines getJobLogs(
    const std::string& logtype,
    const std::vector<std::string>& jobs,
    const std::vector<std::string>& nodes,
    int64_t line_id,
    bool is_ascending,
    int limit,
    const std::string& regex_filter
  );

  void prune() noexcept;

private:
  std::shared_ptr<sqlite::Database> db_;

  folly::Synchronized<std::unordered_map<
    LogTable,
    std::shared_ptr<sqlite::Statement>,
    EnumHash
  >> stmts_;

  folly::Synchronized<
    std::vector<std::shared_ptr<sqlite::Statement>>
  > pruneStmts_;

  std::atomic<uint32_t> counter_;

};

}}
