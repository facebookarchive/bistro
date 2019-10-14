/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/statuses/SQLiteTaskStore.h"
#include "bistro/bistro/utils/TemporaryFile.h"

using namespace facebook::bistro;
using namespace std;

struct StatusCatcher {
  void operator()(
      const string& /*job*/,
      const string& node,
      TaskStore::TaskResult r,
      int64_t timestamp) {
    tasks.push_back(make_tuple(node, r));
  }

  vector<tuple<string, TaskStore::TaskResult>> tasks;
};

TEST(TestSQLiteTaskStore, HandleAll) {
  TemporaryDir db_dir;
  SQLiteTaskStore store(db_dir.getPath(), "statuses");

  StatusCatcher catcher;
  store.fetchJobTasks({"a"}, ref(catcher));
  ASSERT_TRUE(catcher.tasks.empty());

  store.store("a", "node1", TaskStore::TaskResult::DONE);
  store.store("a", "node2", TaskStore::TaskResult::FAILED);
  store.fetchJobTasks({"a"}, ref(catcher));
  ASSERT_EQ(2, catcher.tasks.size());

  const auto& task = catcher.tasks[0];
  ASSERT_EQ("node1", std::get<0>(task));
  ASSERT_EQ(TaskStore::TaskResult::DONE, std::get<1>(task));

  const auto& task2 = catcher.tasks[1];
  ASSERT_EQ("node2", std::get<0>(task2));
  ASSERT_EQ(TaskStore::TaskResult::FAILED, std::get<1>(task2));
}
