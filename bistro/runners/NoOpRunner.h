#pragma once

#include <boost/noncopyable.hpp>
#include <memory>
#include <thread>

#include "bistro/bistro/runners/TaskRunner.h"

namespace facebook { namespace bistro {

class Job;
class Node;
class TaskStatus;

/**
 * A no-op worker runner that just ignores whatever it gets passed in. Useful
 * for debugging the scheduler if you don't actually want to run any jobs.
 */
class NoOpRunner : boost::noncopyable, public TaskRunner {
protected:
 TaskRunnerResponse runTaskImpl(
     const std::shared_ptr<const Job>& job,
     const std::shared_ptr<const Node>& node,
     cpp2::RunningTask& rt,
     folly::dynamic& job_args,
     std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)>
         cb) noexcept override;
};

}}
