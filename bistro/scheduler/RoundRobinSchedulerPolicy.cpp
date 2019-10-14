/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/scheduler/RoundRobinSchedulerPolicy.h"

#include "bistro/bistro/scheduler/utils.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"

namespace facebook { namespace bistro {

using namespace std;

int RoundRobinSchedulerPolicy::schedule(
    vector<JobWithNodes>& jobs,
    TaskRunnerCallback cb) {

  int scheduled_tasks = 0;
  while (!jobs.empty()) {
    for (auto job_it = jobs.begin(); job_it != jobs.end();) {
      if (job_it->nodes.empty()) {
        job_it = jobs.erase(job_it);
        continue;
      }
      while (!job_it->nodes.empty()) {
        const auto ret = try_to_schedule(*job_it->nodes.back(), *job_it, cb);
        job_it->nodes.pop_back();
        if (ret == TaskRunnerResponse::RanTask) {
          ++scheduled_tasks;
          break;
        } else if (ret == TaskRunnerResponse::DoNotRunMoreTasks) {
          return scheduled_tasks;
        }
      }
      ++job_it;
    }
  }
  return scheduled_tasks;
}

}}
