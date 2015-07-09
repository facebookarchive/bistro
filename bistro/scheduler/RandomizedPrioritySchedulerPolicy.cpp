/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/scheduler/RandomizedPrioritySchedulerPolicy.h"

#include <random>

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/scheduler/utils.h"
#include <folly/Memory.h>

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {

struct JobPicker {
  explicit JobPicker(const vector<JobWithNodes>& jobs)
      : gen_(random_device()()) {

    vector<double> priorities;
    transform(
      jobs.begin(),
      jobs.end(),
      back_inserter(priorities),
      [](const JobWithNodes& jwn) { return jwn.job->priority(); }
    );
    d_ = discrete_distribution<>(priorities.begin(), priorities.end());
  }

  int pick() {
    return d_(gen_);
  }

private:
  mt19937 gen_;
  discrete_distribution<> d_;
};

}

int RandomizedPrioritySchedulerPolicy::schedule(
    vector<JobWithNodes>& jobs,
    ResourcesByNodeType& resources_by_node,
    TaskRunnerCallback cb) {

  int scheduled_tasks = 0;
  auto picker = make_unique<JobPicker>(jobs);
  while (!jobs.empty()) {
    const int job_idx = picker->pick();
    JobWithNodes& job_with_nodes = jobs[job_idx];
    if (job_with_nodes.nodes.empty()) {
      jobs.erase(jobs.begin() + job_idx);
      picker.reset(new JobPicker(jobs));
      continue;
    }
    while (!job_with_nodes.nodes.empty()) {
      NodePtr node = job_with_nodes.nodes.back();
      job_with_nodes.nodes.pop_back();
      const auto ret = try_to_schedule(
        resources_by_node,
        node,
        job_with_nodes.job,
        cb
      );
      if (ret == TaskRunnerResponse::RanTask) {
        ++scheduled_tasks;
        break;
      } else if (ret == TaskRunnerResponse::DoNotRunMoreTasks) {
        return scheduled_tasks;
      }
    }
  }
  return scheduled_tasks;
}

}}
