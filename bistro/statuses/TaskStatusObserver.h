#pragma once

#include <string>

#include "bistro/bistro/statuses/TaskStatus.h"

namespace facebook { namespace bistro {

namespace cpp2 {
  class RunningTask;
}

/**
 * Represents an action to perform when we get a task update. This is usually
 * some type of logging.
 */
struct TaskStatusObserver {
  virtual ~TaskStatusObserver() {}

  virtual void updateTaskStatus(
    const cpp2::RunningTask& rt,
    const TaskStatus& status
  ) = 0;

  virtual std::string name() = 0;
};

}}
