/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>

#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/processes/TaskSubprocessQueue.h"
#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Job;
class LogLines;
class Node;
class TaskStatus;

/**
 * Run a task locally on the current machine.
 */
class LocalRunner : public TaskRunner {

public:
  LocalRunner(
    const boost::filesystem::path& cmd,
    const boost::filesystem::path& dir
  );

  LogLines getJobLogs(
    const std::string& logtype,
    const std::vector<std::string>& jobs,
    const std::vector<std::string>& nodes,
    int64_t line_id,
    bool is_ascending,
    const std::string& regex_filter
  ) const override;

  bool canKill() override { return true; }

  void killTask(const cpp2::RunningTask&, const cpp2::KillRequest&) override;

protected:
 TaskRunnerResponse runTaskImpl(
     const std::shared_ptr<const Job>& job,
     const Node& node,
     cpp2::RunningTask& rt,
     folly::dynamic& job_args,
     std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)>
         cb) noexcept override;

private:
  TaskSubprocessQueue taskQueue_;
  boost::filesystem::path cmd_;
  boost::filesystem::path jobsDir_;
};

}}
