/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/sqlite/Database.h"

#include <glog/logging.h>

#include "bistro/bistro/sqlite/Statement.h"

namespace facebook { namespace bistro { namespace sqlite {

namespace {
  std::atomic<bool> sqliteInitDone(false);

  void callback(void* /*ignore*/, int err, const char* msg) {
    LOG(ERROR) << "Sqlite error: (" << err << ") " << msg;
  }

  void initSqlite() {
    if (!sqliteInitDone.exchange(true)) {
      CHECK(sqlite3_threadsafe());
      int r = sqlite3_config(SQLITE_CONFIG_LOG, callback, nullptr);
      if (r != SQLITE_OK) {
        LOG(FATAL) << "Unable to setup sqlite callback: " << r;
      }
    }
  }
}

using namespace std;

shared_ptr<Database> Database::create(
    const boost::filesystem::path& filename,
    int flags) {
  return make_shared<Database>(DoNotUseThisConstructor(), filename, flags);
}

Database::Database(
    DoNotUseThisConstructor,
    const boost::filesystem::path& filename,
    int flags) {
  initSqlite();
  int ret = sqlite3_open_v2(filename.c_str(), &db_, flags, nullptr);
  if (ret != SQLITE_OK) {
    string err(error());
    sqlite3_close(db_);
    throw Exception("Could not open DB ", filename.native(), ": ", err);
  }
}

Database::~Database() {
  if (sqlite3_close(db_) != SQLITE_OK) {
    LOG(ERROR)
      << "Failed to close sqlite connection: "
      << sqlite3_errmsg(db_);
  }
}

int Database::lastNumChangedRows() {
  return sqlite3_changes(db_);
}

string Database::error() const {
  return sqlite3_errmsg(db_);
}

void Database::throwError() const {
  throwError(error());
}

void Database::throwError(const string& msg) const {
  throw Exception(msg);
}

void Database::exec(const string& sql) {
  Statement(shared_from_this(), sql).exec();
}

shared_ptr<Statement> Database::prepare(const string& sql) {
  return shared_ptr<Statement>(new Statement(shared_from_this(), sql));
}

}}}
