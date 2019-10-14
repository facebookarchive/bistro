/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/LogWriter.h"

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <folly/Conv.h>
#include <folly/experimental/AutoTimer.h>
#include <folly/String.h>

#include "bistro/bistro/utils/Exception.h"
#include "bistro/bistro/utils/LogLines.h"
#include "bistro/bistro/sqlite/Database.h"
#include "bistro/bistro/sqlite/Statement.h"

DEFINE_int32(log_retention, 86400 * 30, "How long to keep worker logs");
DEFINE_int32(log_prune_frequency, 3600, "How frequently to prune task logs");

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {

map<LogTable, const char*> tables = {
  { LogTable::STDERR, "stderr" },
  { LogTable::STDOUT, "stdout" },
  { LogTable::EVENTS, "statuses" },
};

}

LogWriter::LogWriter(const boost::filesystem::path& db_file) : counter_(0) {
  boost::filesystem::create_directories(db_file.parent_path().native());
  db_ = sqlite::Database::create(db_file);

  // Improve performance
  db_->exec("PRAGMA synchronous = NORMAL;");
  db_->exec("PRAGMA journal_mode = WAL;");

  // Set up tables
  for (const auto& pair : tables) {
    const string& name = pair.second;
    db_->exec(to<string>(
      "CREATE TABLE IF NOT EXISTS ", name, " ("
        "job_id TEXT, "
        "node_id TEXT, "
        "time_and_count INTEGER, "
        "line TEXT, "
        "PRIMARY KEY(time_and_count) "
      ")"
    ));

    // According to Andrey's tests, the primary key guarantees we can search
    // slices of this index by time_and_count also.
    db_->exec(to<string>(
      "CREATE INDEX IF NOT EXISTS ", name, "_job_node_idx "
      "ON ", name, " (job_id, node_id)"
    ));
    SYNCHRONIZED(stmts_) {
      stmts_[pair.first] = db_->prepare(to<string>(
        "INSERT INTO ", name, " VALUES(?, ?, ?, ?)"
      ));
    }
    SYNCHRONIZED(pruneStmts_) {
      pruneStmts_.emplace_back(db_->prepare(to<string>(
        "DELETE FROM ", name, " WHERE time_and_count < ?"
      )));
    }

    LOG(INFO) << "Created table " << name;
  }
  // CAUTION: ThreadedRepeatingFunctionRunner recommends two-stage
  // initialization for starting threads.  This specific case is safe since:
  //  - this comes last in the constructor, so the class is fully constructed,
  //  - this class is final, so no derived classes remain to be constructed.
  if (FLAGS_log_prune_frequency > 0) {
    threads_.add("LogWriterPrune", [this]() {
      prune();
      return std::chrono::seconds(FLAGS_log_prune_frequency);
    }, std::chrono::milliseconds(0));
  }
}

LogWriter::~LogWriter() {
  threads_.stop();
}

void LogWriter::write(
    LogTable table,
    const string& job,
    const string& node,
    folly::StringPiece line) {

  const char* table_name = tables.find(table)->second;
  // We might 'invert' the order of lines if counter_ resets (every 4 billion
  // lines). Since this is unlikely to matter we don't bother to account for it.
  auto now = LogLine::makeLineID(time(nullptr), counter_.fetch_add(1));
  SYNCHRONIZED(stmts_) {
    auto it = stmts_.find(table);
    CHECK(it != stmts_.end());
    it->second->exec(job, node, now, line);
  }
}

void LogWriter::prune() noexcept {
  int32_t cutoff_time = time(nullptr) - FLAGS_log_retention;
  auto cutoff =
    LogLine::makeLineID(cutoff_time, numeric_limits<int32_t>::max());
  folly::AutoTimer<> timer(
      folly::to<std::string>("Pruned logs with cutoff ", cutoff_time));
  SYNCHRONIZED(pruneStmts_) {
    for (auto& s : pruneStmts_) {
      s->exec(cutoff);
    }
  }
}

LogLines LogWriter::getJobLogs(
    const string& logtype,
    const vector<string>& jobs,
    const vector<string>& nodes,
    int64_t line_id,
    bool is_ascending,
    int limit,
    const std::string& regex_filter) const {

  if (logtype != "stderr" && logtype != "stdout" && logtype != "statuses") {
    throw BistroException("Unknown table for logs: ", logtype);
  }

  // Compose the WHERE clause -- either of "jobs" or "nodes" may be empty
  vector<string> where_clauses;
  if (!jobs.empty()) {
    where_clauses.emplace_back("job_id IN (");
    for (const auto& job : jobs) {
      where_clauses.back().append("?,");
    }
    where_clauses.back().back() = ')';
  }
  if (!nodes.empty()) {
    where_clauses.emplace_back("node_id IN (");
    for (const auto& node : nodes) {
      where_clauses.back().append("?,");
    }
    where_clauses.back().back() = ')';
  }
  if (line_id != LogLine::kNotALineID) {
    if (is_ascending) {
      where_clauses.emplace_back(folly::to<string>(
        "time_and_count >= ", line_id
      ));
    } else {
      where_clauses.emplace_back(folly::to<string>(
        "time_and_count <= ", line_id
      ));
    }
  }
  string where_clause;
  if (!where_clauses.empty()) {
    where_clause = "WHERE (" + folly::join(") AND (", where_clauses) + ")";
  }

  string query = folly::to<string>(
    "SELECT job_id, node_id, time_and_count, line FROM ", logtype, " ",
    // Sorting only by time_and_count gives us a reasonable ability to view
    // logs across multiple jobs or nodes, and uses the primary index.
    where_clause, " ORDER BY time_and_count ", (is_ascending ? "ASC" : "DESC"),
    " LIMIT ", limit + 1
  );

  // Bind the WHERE clause arguments
  auto st = db_->prepare(query);
  auto bind_it = st->bindIterator();
  string debug_where_args{"'; args: '"};  // WHERE args for the logs
  for (const auto& job : jobs) {
    *bind_it++ = job;
    debug_where_args.append(job);
    debug_where_args.append("', '");
  }
  for (const auto& node : nodes) {
    *bind_it++ = node;
    debug_where_args.append(node);
    debug_where_args.append("', '");
  }

  // Run the query
  folly::AutoTimer<>(
      folly::to<std::string>("Query: '", query, debug_where_args));
  LogLines res;
  // Assuming that micro-optimizing the "" case is pointless, but did not test.
  boost::regex re(regex_filter);
  for (const auto& r : st->query()) {
    const auto& line = r.getText(3);
    if (!boost::regex_search(line, re)) {
      continue;
    }
    auto id = r.getInt64(2);
    res.lines.emplace_back(
      r.getText(0), r.getText(1), LogLine::timeFromLineID(id), line, id
    );
  }
  if (res.lines.size() <= limit) {
    // There are no more lines in this direction. This sentinel tells
    // WorkerRunner::getJobLogs that this host does not constrain the
    // aggregate nextLineID.
    res.nextLineID = LogLine::kNotALineID;
  } else {
    res.nextLineID = res.lines.back().lineID;
    res.lines.pop_back();
  }
  LOG(INFO) << "Got " << res.lines.size() << " " << logtype << " lines";
  return res;
}

}}
