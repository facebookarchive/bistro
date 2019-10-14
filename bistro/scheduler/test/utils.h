/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <limits>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/runners/TaskRunner.h"
#include "bistro/bistro/scheduler/SchedulerPolicy.h"

namespace facebook { namespace bistro {

struct TaskCatcher {
  TaskRunnerResponse operator()(const JobPtr& job, const Node& node) {
    tasks.push_back(make_pair(job->name(), node.name()));
    return RanTask;
  }

  bool contains(std::pair<std::string, std::string> task) {
    return tasks.end() != std::find(tasks.begin(), tasks.end(), task);
  }

  std::vector<std::pair<std::string, std::string>> tasks;
  int numTasks;
};

template<class Scheduler>
TaskCatcher mock_schedule(bool add_third_job = false) {
  Config config(folly::dynamic::object
    ("nodes", folly::dynamic::object
      ("levels", folly::dynamic::array("host", "db"))
      ("node_sources", folly::dynamic::array(
        folly::dynamic::object("source", "range_label")
      ))
    )
    ("resources", folly::dynamic::object
      ("host", folly::dynamic::object
        ("host_concurrency", folly::dynamic::object
          ("default", 1)
          ("limit", 2)
        )
      )
      ("db", folly::dynamic::object
        ("db_concurrency", folly::dynamic::object
          ("default", 1)
          ("limit", 1)
        )
      )
    )
  );
  config.addJob(
    std::make_shared<Job>(
        config,
        "job1",
        folly::dynamic::object
          ("owner", "owner")
          ("priority", 10.0)),
    nullptr
  );
  config.addJob(
    std::make_shared<Job>(
        config,
        "job2",
        folly::dynamic::object
          ("owner", "owner")
          ("priority", 1.0)),
    nullptr
  );
  if (add_third_job) {
    config.addJob(
      std::make_shared<Job>(
          config,
          "job3",
        folly::dynamic::object
          ("owner", "owner")
          ("priority", 100.0)
          ("filters", folly::dynamic::object
            ("db", folly::dynamic::object
              ("whitelist", folly::dynamic::array("db12"))
            )
          )),
      nullptr
    );
  }

  auto host1 = std::make_shared<Node>("host1", 1, true);
  host1->offset = 0;
  auto host2 = std::make_shared<Node>("host2", 1, true);
  host2->offset = 1;
  Node db11("db11", 2, true, host1.get());
  db11.offset = 0;
  Node db12("db12", 2, true, host1.get());
  db12.offset = 1;
  Node db21("db21", 2, true, host2.get());
  db21.offset = 2;
  Node db22("db22", 2, true, host2.get());
  db22.offset = 3;

  NodeGroupToPackedResources resources = {
    {1, {2, 2}},
    {2, {1, 1, 1, 1}},
  };

  std::vector<JobWithNodes> jobs;
  jobs.emplace_back(config, config.jobs["job1"], &resources);
  jobs.back().nodes = {&db11, &db12, &db21, &db22};
  jobs.emplace_back(config, config.jobs["job2"], &resources);
  jobs.back().nodes = {&db11, &db12, &db21, &db22};
  if (add_third_job) {
    jobs.emplace_back(config, config.jobs["job3"], &resources);
    jobs.back().nodes = {&db12};
  }
  TaskCatcher catcher;
  catcher.numTasks = Scheduler().schedule(jobs, std::ref(catcher));
  return catcher;
}

}}
