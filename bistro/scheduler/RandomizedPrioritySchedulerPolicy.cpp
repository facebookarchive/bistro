/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
      [](const JobWithNodes& jwn) { return jwn.job()->priority(); }
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
    TaskRunnerCallback cb) {

  int scheduled_tasks = 0;
  auto picker = make_unique<JobPicker>(jobs);
  while (!jobs.empty()) {
    const int job_idx = picker->pick();
    JobWithNodes& jwn = jobs[job_idx];
    if (jwn.nodes.empty()) {
      jobs.erase(jobs.begin() + job_idx);
      picker.reset(new JobPicker(jobs));
      continue;
    }
    while (!jwn.nodes.empty()) {
      const auto ret = try_to_schedule(*jwn.nodes.back(), jwn, cb);
      jwn.nodes.pop_back();
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
