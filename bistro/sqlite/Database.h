/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <folly/Conv.h>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace facebook { namespace bistro { namespace sqlite {

struct Exception : public std::runtime_error {
  template <typename... Args>
  explicit Exception(Args&&... args) : std::runtime_error(
    folly::to<std::string>(std::forward<Args>(args)...)
  ) {}
};

class Statement;

/**
 * Lightweight wrapper around the sqlite C API.
 */
class Database : public std::enable_shared_from_this<Database> {
private:
  struct DoNotUseThisConstructor {};

public:
  /**
   * We only support creating a connection via shared_ptr. Otherwise if you had
   * a statement and a database, you would have to destroy them in the correct
   * order, since all statements must be finalized before the db object can be
   * destroyed.
   */
  static std::shared_ptr<Database> create(
    const boost::filesystem::path& file,
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
  );

  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&&) = delete;
  Database& operator=(Database&&) = delete;

  void exec(const std::string& sql);
  std::shared_ptr<Statement> prepare(const std::string& sql);

  int lastNumChangedRows();  // sqlite3_changes

  // Public only for std::make_shared
  Database(
    DoNotUseThisConstructor,
    const boost::filesystem::path& filename,
    int flags
  );

private:
  friend class Statement;

  std::string error() const;
  void throwError() const;
  void throwError(const std::string& msg) const;

  sqlite3* db_;

};

}}}
