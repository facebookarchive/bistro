/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
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
