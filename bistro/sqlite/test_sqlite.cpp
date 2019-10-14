/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bistro/bistro/sqlite/Database.h"
#include "bistro/bistro/sqlite/Statement.h"

using namespace facebook::bistro::sqlite;

std::shared_ptr<Database> init() {
  auto db = Database::create(":memory:");
  db->exec("CREATE TABLE tmp (int integer, int64 bigint, str text)");
  return db;
}

TEST(TestSQLite, TestCreateInsertSelect) {
  auto db = init();
  db->exec("INSERT INTO tmp VALUES (1, 64, \"sheep\")");
  db->exec("INSERT INTO tmp VALUES (2, 65, \"cow\")");
  db->exec("INSERT INTO tmp VALUES (3, 66, \"goat\")");

  auto st = db->prepare("SELECT * FROM tmp");

  // Unroll the loop so we can manually check each row
  auto h = st->query();
  auto it = h.begin();
  ASSERT_FALSE(it == h.end());
  ASSERT_EQ(1, it->getInt(0));
  ASSERT_EQ(64, it->getInt64(1));
  ASSERT_EQ("sheep", it->getText(2));

  ASSERT_FALSE(++it == h.end());
  ASSERT_EQ(2, it->getInt(0));
  ASSERT_EQ(65, it->getInt64(1));
  ASSERT_EQ("cow", it->getText(2));

  ASSERT_FALSE(++it == h.end());
  ASSERT_EQ(3, it->getInt(0));
  ASSERT_EQ(66, it->getInt64(1));
  ASSERT_EQ("goat", it->getText(2));

  ASSERT_TRUE(++it == h.end());

  // More typical usage
  std::vector<int> v;
  for (auto& row : st->query()) {
    v.push_back(row.getInt(0));
  }
  ASSERT_EQ((std::vector<int>{1, 2, 3}), v);

  // Use tuples
  typedef std::tuple<int, int64_t, std::string> RowTuple;
  std::vector<RowTuple> w;
  int id = 0;
  for (RowTuple row : st->query()) {
    ASSERT_EQ(++id, std::get<0>(row));
    w.emplace_back(row);
  }
  decltype(w) expected{
    RowTuple{ 1, 64, "sheep" },
    RowTuple{ 2, 65, "cow" },
    RowTuple{ 3, 66, "goat" },
  };
  ASSERT_EQ(expected, w);

  // Select a single value
  id = 0;
  for (int row_id : st->query()) {
    ASSERT_EQ(++id, row_id);
  }

  auto count_st = db->prepare("SELECT COUNT(1) FROM tmp");
  int c = count_st->querySingleResult<int>();
  ASSERT_EQ(3, c);

  auto count_st2 = db->prepare("SELECT COUNT(1) FROM tmp WHERE str = ?");
  c = count_st2->querySingleResult<int>("goat");
  ASSERT_EQ(1, c);

  // Should throw because this returns multiple results
  ASSERT_THROW(({c = st->querySingleResult<int>();}), Exception);
}

TEST(TestSQLite, TestBind) {
  auto db = init();
  auto st = db->prepare("INSERT INTO tmp VALUES (?, ?, ?)");
  st->exec(5, 55, "dog");
  st->exec(6, 55, "dog");
  st->exec(6, 66, "cat");

  // Test bind iterator
  auto bit = st->bindIterator();
  *bit++ = 7;
  *bit++ = 55;
  *bit++ = "moo";
  ASSERT_THROW(({*bit = 99;}), Exception);
  st->exec();

  auto range = db->prepare("SELECT * FROM tmp WHERE int64 = ?")->query(55);
  auto it = range.begin();
  ASSERT_FALSE(it == range.end());
  ASSERT_EQ(5, it->getInt(0));
  ASSERT_EQ(6, (++it)->getInt(0));
  ASSERT_EQ(7, (++it)->getInt(0));
  ASSERT_EQ(++it, range.end());
}

TEST(TestSQLite, TestInvalidParameterCount) {
  auto db = init();
  auto st = db->prepare("INSERT INTO tmp VALUES (?, ?, ?)");
  ASSERT_THROW(st->exec(5, 6, 7, 8), Exception);
}

TEST(TestSQLite, TestTooManyColumns) {
  auto db = init();
  db->exec("INSERT INTO tmp VALUES (1, 64, \"sheep\")");
  auto st = db->prepare("SELECT * FROM tmp");
  ASSERT_THROW(({
    for (std::tuple<int, int, int, int, int> t : st->query()) {
      ASSERT_EQ(-1, std::get<0>(t)); // avoid compiler error by using t
    }
  }), Exception);
}

TEST(TestSQLite, TestInvalidType) {
  auto db = init();
  db->exec("INSERT INTO tmp VALUES (1, 64, \"sheep\")");
  auto st = db->prepare("SELECT * FROM tmp");
  ASSERT_THROW(({
    for (std::tuple<int, int, int> t : st->query()) {
      ASSERT_EQ(-1, std::get<0>(t)); // avoid compiler error by using t
    }
  }), Exception);
}
