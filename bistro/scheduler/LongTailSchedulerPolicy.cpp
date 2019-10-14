/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/scheduler/LongTailSchedulerPolicy.h"

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/scheduler/utils.h"

namespace facebook { namespace bistro {

using namespace std;

int LongTailSchedulerPolicy::schedule(
    vector<JobWithNodes>& jobs,
    TaskRunnerCallback cb) {

  sort(jobs.begin(), jobs.end(),
    [](const JobWithNodes& a, const JobWithNodes& b) {
      return a.nodes.size() < b.nodes.size();
    }
  );

  int scheduled_tasks = 0;
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
      } else if (ret == TaskRunnerResponse::DoNotRunMoreTasks) {
        return scheduled_tasks;
      }
    }
    ++job_it;
  }
  return scheduled_tasks;
}

}}
