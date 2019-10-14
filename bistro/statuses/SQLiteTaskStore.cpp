/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/statuses/SQLiteTaskStore.h"

#include <folly/Conv.h>

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

SQLiteTaskStore::SQLiteTaskStore(
  const boost::filesystem::path& db_dir,
  const string& table)
    : table_(table) {

  // Can't have '.' in table names without escaping
  replace(table_.begin(), table_.end(), '.', '_');

  boost::filesystem::path dir(db_dir);
  db_ = sqlite::Database::create(dir / "statuses.sql3");

  // Improve performance
  db_->exec("PRAGMA synchronous = NORMAL;");
  db_->exec("PRAGMA journal_mode = WAL;");

  // Create the statuses table if it doesn't exist already.
  db_->exec(to<string>(
    "CREATE TABLE IF NOT EXISTS ", table_, " ("
      "job_id TEXT, "
      "node_id TEXT, "
      "timestamp INTEGER, "
      "data TEXT, "
      "result INTEGER, "
      "PRIMARY KEY(job_id, node_id) "
    ")"
  ));

  SYNCHRONIZED(stmt_) {
    stmt_ = db_->prepare(to<string>(
      "INSERT OR REPLACE INTO ", table_, " (job_id, node_id, result, timestamp)"
      " VALUES (?, ?, ?, ?)"
    ));
  }
}

void SQLiteTaskStore::fetchJobTasks(
    const std::vector<std::string>& job_ids, Callback cb) {

  for (const auto& job_id : job_ids) {
    // TODO: Use "IN" query with up to 999 job IDs (SQLite limitation)
    auto st = db_->prepare(to<string>(
      "SELECT job_id, node_id, result, timestamp FROM ",
      table_,
      " WHERE job_id = ?"
    ));
    for (const tuple<string, string, int, int64_t>& row : st->query(job_id)) {
      cb(
        get<0>(row),
        get<1>(row),
        static_cast<TaskResult>(get<2>(row)),
        get<3>(row));
    }
  }
}

void SQLiteTaskStore::store(
    const string& job,
    const string& node,
    TaskResult r) {
  SYNCHRONIZED(stmt_) {
    stmt_->exec(job, node, r, time(nullptr));
  }
}

}}
