/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/utils/LogWriter.h"
#include "bistro/bistro/sqlite/Database.h"
#include "bistro/bistro/sqlite/Statement.h"

using namespace facebook;
using namespace facebook::bistro;
using namespace std;

DECLARE_int32(log_retention);
DECLARE_int32(log_prune_frequency);

// Copypasta'd from LogWriter.cpp because C++ is too hard.
map<LogTable, string> tables = {
  { LogTable::STDERR, "stderr" },
  { LogTable::STDOUT, "stdout" },
  { LogTable::STATUSES, "statuses" },
};

TEST(TestLogWriter, HandleAll) {
  FLAGS_log_prune_frequency = 0;

  vector<tuple<LogTable, string, string, vector<string>>> inputs{
    make_tuple(
      LogTable::STDOUT, "job1", "node1", vector<string>{"line1", "line2"}),
    make_tuple(
      LogTable::STDERR, "job2", "node2", vector<string>{"line3", "line4"}),
    make_tuple(
      LogTable::STATUSES, "job3", "node3", vector<string>{"line5", "line6"}),
  };

  TemporaryFile db_file;
  LogWriter writer(db_file.getFilename());
  for (const auto& d : inputs) {
    for (const auto& line : std::get<3>(d)) {
      writer.write(std::get<0>(d), std::get<1>(d), std::get<2>(d), line);
    }
  }

  auto now = time(nullptr);
  auto db = sqlite::Database::create(db_file.getFilename());
  for (const auto& d : inputs) {
    auto job = std::get<1>(d);
    auto node = std::get<2>(d);
    auto s = db->prepare("SELECT * FROM " + tables[std::get<0>(d)]);
    auto result = s->query();
    auto it = result.begin();
    for (const auto& line : std::get<3>(d)) {
      ASSERT_NE(it, result.end());
      ASSERT_EQ(job, it->getText(0));
      ASSERT_EQ(node, it->getText(1));
      int s_time = LogLine::timeFromLineID(it->getInt64(2));
      ASSERT_GE(now, s_time);
      ASSERT_LE(s_time, now + 2);
      ASSERT_EQ(line, it->getText(3));
      ++it;
    }
    ASSERT_EQ(it, result.end());
  }

  // Check log pruning
  FLAGS_log_retention = 0;
  writer.prune();
  for (const auto& table : tables) {
    for (auto& row : db->prepare("SELECT * FROM " + table.second)->query()) {
      FAIL();  // should have no rows
    }
  }
}
