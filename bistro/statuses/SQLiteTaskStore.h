/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>

#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/sqlite/Database.h"
#include "bistro/bistro/sqlite/Statement.h"
#include <folly/Synchronized.h>

namespace facebook { namespace bistro {

class SQLiteTaskStore : public TaskStore {

public:
  SQLiteTaskStore(
    const boost::filesystem::path& db_dir,
    const std::string& table
  );

  void fetchJobTasks(const std::vector<std::string>& job_ids,
                     Callback cb) override;

  /**
   * Since writing to SQLite is fast enough, this call blocks till the insert
   * query returns.
   */
  void store(const std::string& job,
             const std::string& node,
             TaskResult r) override;

private:
  std::string table_;
  std::shared_ptr<sqlite::Database> db_;

  folly::Synchronized<std::shared_ptr<sqlite::Statement>> stmt_;
};

}}
