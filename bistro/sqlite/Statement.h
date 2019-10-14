/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/function_output_iterator.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/range/iterator_range.hpp>
#include <glog/logging.h>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <tuple>

#include <folly/Range.h>

namespace facebook { namespace bistro { namespace sqlite {

class Database;
class Statement;
class StatementIterator;

class Row {
public:

  int32_t getInt(int id) const;
  int64_t getInt64(int id) const;
  double getDouble(int id) const;
  std::string getText(int id) const;

  template<typename... Args>
  std::tuple<Args...> asTuple() const {
    check(sizeof...(Args));
    return asTupleImpl<Args...>(0);
  }

  template<typename... Args>
  /* implicit */ operator std::tuple<Args...>() const {
    check(sizeof...(Args));
    return asTupleImpl<Args...>(0);
  }

  template<typename T>
  /* implicit */ operator T() const {
    T t;
    get(0, &t);
    return t;
  }

private:
  template<typename T, typename S, typename... Args>
  std::tuple<T, S, Args...> asTupleImpl(int id) const {
    T t;
    get(id, &t);
    return std::tuple_cat(
      std::tuple<T>(std::move(t)),
      asTupleImpl<S, Args...>(id + 1)
    );
  }

  template<typename T>
  std::tuple<T> asTupleImpl(int id) const {
    T t;
    get(id, &t);
    return std::tuple<T>(std::move(t));
  }

  void get(int id, int* out) const {
    *out = getInt(id);
  }
  void get(int id, int64_t* out) const {
    *out = getInt64(id);
  }
  void get(int id, double* out) const {
    *out = getDouble(id);
  }
  void get(int id, std::string* out) const {
    *out = getText(id);
  }
  void check(int num_cols) const;

  friend class StatementIterator;
  friend class Statement;

  explicit Row(Statement* s);

  Statement* stmt_;
};

class StatementIterator : public boost::iterator_facade<
                            StatementIterator,
                            const Row,
                            boost::forward_traversal_tag> {
public:
  StatementIterator();
  explicit StatementIterator(std::shared_ptr<Statement> s);

private:
  friend class boost::iterator_core_access;

  bool equal(const StatementIterator& other) const;
  const Row& dereference() const;
  void increment();

  std::shared_ptr<Statement> stmt_;
  Row row_;
};

class StatementBinder {

public:
  void operator()(int32_t val);
  void operator()(int64_t val);
  void operator()(double val);
  void operator()(folly::StringPiece val);

private:
  friend class Statement;

  explicit StatementBinder(Statement* stmt)
    : pos(1), stmt_(stmt) {}

  int pos;
  Statement* stmt_;

};

class Statement : public std::enable_shared_from_this<Statement> {
public:
  ~Statement();

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;

  /**
   * Execute the statement, binding in the passed-in arguments. Useful for
   * queries that don't return any results.
   */
  template<typename... Args>
  void exec(Args&&... args) {
    if (sizeof...(Args) > sqlite3_bind_parameter_count(stmt_)) {
      throwError("Too many arguments passed");
    }
    maybeReset();
    bindAll(1, std::forward<Args>(args)...);
    step();
  }

  /**
   * Run the statement, binding in the passed-in arguments.
   */
  template<typename... Args>
  boost::iterator_range<StatementIterator> query(Args&&... args) {
    if (sizeof...(Args) > sqlite3_bind_parameter_count(stmt_)) {
      throwError("Too many arguments passed");
    }
    maybeReset();
    bindAll(1, std::forward<Args>(args)...);
    return queryImpl();
  }

  /**
   * Run the statement, returning a single value as a result.
   */
  template<typename T, typename... Args>
  T querySingleResult(Args&&... args) {
    exec(std::forward<Args>(args)...);
    T result(Row(this)); // Create a copy of the data
    if (step()) {
      throwError("querySingleResult() got more than 1 row of results");
    }
    return result;
  }


  boost::function_output_iterator<StatementBinder> bindIterator() {
    maybeReset();
    return boost::make_function_output_iterator(StatementBinder(this));
  }

private:
  friend class Database;
  friend class Row;
  friend class StatementIterator;
  friend class StatementBinder;

  std::shared_ptr<Database> db_;
  sqlite3_stmt* stmt_;
  bool needsReset_;

  Statement(std::shared_ptr<Database> db, const std::string& sql);

  void check(int ret) const;
  boost::iterator_range<StatementIterator> queryImpl();
  bool step();
  void maybeReset();

  template<typename T, typename... Args>
  void bindAll(int id, T&& t, Args&&... args) {
    bind(id, t);
    bindAll(id + 1, args...);
  }

  void throwError(const std::string& msg) const;

  void bindAll(int /*id*/) {}

  inline void bind(int id, int32_t value) {
    check(sqlite3_bind_int(stmt_, id, value));
  }

  inline void bind(int id, int64_t value) {
    check(sqlite3_bind_int64(stmt_, id, value));
  }

  inline void bind(int id, double value) {
    check(sqlite3_bind_double(stmt_, id, value));
  }

  inline void bind(int id, folly::StringPiece s) {
    //  Using SQLITE_STATIC here means the passed in string must exist at least
    //  as long as we're executing the statement. Thus we take a
    //  folly::StringPiece as the argument, since that doesn't convey any
    //  ownership semantics.
    check(sqlite3_bind_text(stmt_, id, s.start(), s.size(), SQLITE_STATIC));
  }

};

}}}
