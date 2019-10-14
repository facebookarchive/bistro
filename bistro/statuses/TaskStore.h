/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <string>

namespace facebook { namespace bistro {

/**
 * Represents a store of finished/failed tasks. Used on startup to fetch task
 * state so that we don't re-run finished/failed tasks.
 *
 * Your implementation must be thread-safe.
 */
struct TaskStore {

  enum TaskResult {
    // Do NOT change these, as persisted data will still have the old values.
    DONE = 1,
    FAILED = 2,
  };

  typedef std::function<
    void(
        const std::string& job_id,
        const std::string& node_id,
        TaskResult,
        int64_t timestamp)
  > Callback;

  virtual ~TaskStore() {}

  virtual void fetchJobTasks(
    const std::vector<std::string>& job_ids,
    Callback cb
  ) = 0;

  virtual void store(const std::string&, const std::string&, TaskResult) = 0;

};

/**
 * A "store" that doesn't do anything. Useful if you don't want to actually
 * persist done state anywhere.
 */
struct NoOpTaskStore : public TaskStore {
  void fetchJobTasks(
      const std::vector<std::string>& /*job_ids*/,
      Callback /*cb*/) override {}

  void store(const std::string&, const std::string&, TaskResult) override {}
};

}}
