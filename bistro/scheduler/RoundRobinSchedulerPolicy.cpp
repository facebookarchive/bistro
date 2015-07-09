/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/RoundRobinSchedulerPolicy.h"

#include "bistro/bistro/scheduler/utils.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"

namespace facebook { namespace bistro {

using namespace std;

int RoundRobinSchedulerPolicy::schedule(
    vector<JobWithNodes>& jobs,
    ResourcesByNodeType& resources_by_node,
    TaskRunnerCallback cb) {

  int scheduled_tasks = 0;
  while (!jobs.empty()) {
    for (auto job_it = jobs.begin(); job_it != jobs.end();) {
      if (job_it->nodes.empty()) {
        job_it = jobs.erase(job_it);
        continue;
      }
      const JobPtr& job = job_it->job;
      while (!job_it->nodes.empty()) {
        NodePtr node = job_it->nodes.back();
        job_it->nodes.pop_back();
        const auto ret = try_to_schedule(
          resources_by_node,
          node,
          job,
          cb
        );
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
