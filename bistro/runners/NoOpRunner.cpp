/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/runners/NoOpRunner.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/statuses/TaskStatus.h"

namespace facebook { namespace bistro {

TaskRunnerResponse NoOpRunner::runTaskImpl(
  const std::shared_ptr<const Job>&,
  const Node&,
  cpp2::RunningTask& rt,
  folly::dynamic& job_args,
  std::function<void(const cpp2::RunningTask& rt, TaskStatus&& status)> cb
) noexcept {
  cb(rt, TaskStatus::running());
  if (!lastStatus_.isRunning()) {
    cb(rt, TaskStatus(lastStatus_));
  }
  return RanTask;
}

}}
