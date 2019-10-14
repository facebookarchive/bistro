/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <memory>
#include <thread>

#include "bistro/bistro/runners/TaskRunner.h"
#include "bistro/bistro/statuses/TaskStatus.h"

namespace facebook { namespace bistro {

class Job;
class Node;
class TaskStatus;

/**
 * A no-op worker runner that just ignores whatever it gets passed in. Useful
 * for debugging the scheduler if you don't actually want to run any jobs.
 */
class NoOpRunner : boost::noncopyable, public TaskRunner {
public:
  explicit NoOpRunner(TaskStatus last_status = TaskStatus::done())
    : lastStatus_(std::move(last_status)) {}
protected:
  TaskRunnerResponse runTaskImpl(
    const std::shared_ptr<const Job>& job,
    const Node& node,
    cpp2::RunningTask& rt,
    folly::dynamic& job_args,
    std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
  ) noexcept override;
private:
  TaskStatus lastStatus_;
};

}}
