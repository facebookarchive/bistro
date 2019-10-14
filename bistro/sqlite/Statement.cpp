/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/sqlite/Statement.h"

#include <glog/logging.h>

#include "bistro/bistro/sqlite/Database.h"

namespace facebook { namespace bistro { namespace sqlite {

using namespace std;

Statement::Statement(shared_ptr<Database> db, const string& sql)
    : db_(db), needsReset_(false) {
  int ret = sqlite3_prepare_v2(db_->db_, sql.c_str(), -1, &stmt_, nullptr);
  if (ret != SQLITE_OK) {
    string err(db_->error());
    sqlite3_finalize(stmt_);
    throw Exception("Could not prepare statement ", sql, ": ", err);
  }
}

Statement::~Statement() {
  if (sqlite3_finalize(stmt_) != SQLITE_OK) {
    LOG(ERROR) << "Failed to finalize statement: " << db_->error();
  }
}

boost::iterator_range<StatementIterator> Statement::queryImpl() {
  return boost::make_iterator_range(
    step() ? StatementIterator(shared_from_this()) : StatementIterator(),
    StatementIterator()
  );
}

bool Statement::step() {
  int ret = sqlite3_step(stmt_);
  if (ret != SQLITE_DONE && ret != SQLITE_ROW) {
    db_->throwError();
  }
  if (ret == SQLITE_ROW) {
    return true;
  } else {
    needsReset_ = true;
    return false;
  }
}

void Statement::check(int ret) const {
  if (ret != SQLITE_OK) {
    throw Exception(db_->error());
  }
}

void Statement::maybeReset() {
  if (needsReset_) {
    sqlite3_reset(stmt_);
    needsReset_ = false;
  }
}

void Statement::throwError(const string& msg) const {
  db_->throwError(msg);
}

Row::Row(Statement* s) : stmt_(s) {}

int32_t Row::getInt(int id) const {
  if (sqlite3_column_type(stmt_->stmt_, id) != SQLITE_INTEGER) {
    stmt_->throwError("Column is not an integer");
  }
  return sqlite3_column_int(stmt_->stmt_, id);
}

int64_t Row::getInt64(int id) const {
  if (sqlite3_column_type(stmt_->stmt_, id) != SQLITE_INTEGER) {
    stmt_->throwError("Column type is not integer");
  }
  return sqlite3_column_int64(stmt_->stmt_, id);
}

double Row::getDouble(int id) const {
  if (sqlite3_column_type(stmt_->stmt_, id) != SQLITE_FLOAT) {
    stmt_->throwError("Column type is not float");
  }
  return sqlite3_column_double(stmt_->stmt_, id);
}

string Row::getText(int id) const {
  if (sqlite3_column_type(stmt_->stmt_, id) != SQLITE_TEXT) {
    stmt_->throwError("Column type is not text");
  }
  return string(
    (char*)sqlite3_column_text(stmt_->stmt_, id),
    sqlite3_column_bytes(stmt_->stmt_, id)
  );
}

void Row::check(int num_cols) const {
  if (sqlite3_column_count(stmt_->stmt_) < num_cols) {
    stmt_->throwError("Trying to query for too many columns");
  }
}

StatementIterator::StatementIterator() : StatementIterator(nullptr) {}

StatementIterator::StatementIterator(shared_ptr<Statement> s)
  : stmt_(s), row_(s.get()) {}

bool StatementIterator::equal(const StatementIterator& other) const {
  return stmt_ == other.stmt_;
}

const Row& StatementIterator::dereference() const {
  DCHECK(stmt_);
  return row_;
}

void StatementIterator::increment() {
  if (!stmt_->step()) {
    stmt_ = nullptr;
  }
}

void StatementBinder::operator()(int32_t val) {
  stmt_->bind(pos++, val);
}

void StatementBinder::operator()(int64_t val) {
  stmt_->bind(pos++, val);
}

void StatementBinder::operator()(double val) {
  stmt_->bind(pos++, val);
}

void StatementBinder::operator()(folly::StringPiece val) {
  stmt_->bind(pos++, val);
}

}}}
