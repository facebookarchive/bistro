/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
  TaskRunnerResponse operator()(const JobPtr& job, const NodePtr& node) {
    tasks.push_back(make_pair(job->name(), node->name()));
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
      ("levels", { "host", "db" })
      ("node_source", "range_label")
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
    "job1",
    folly::dynamic::object
      ("owner", "owner")
      ("priority", 10.0),
    nullptr
  );
  config.addJob(
    "job2",
    folly::dynamic::object
      ("owner", "owner")
      ("priority", 1.0),
    nullptr
  );
  if (add_third_job) {
    config.addJob(
      "job3",
      folly::dynamic::object
        ("owner", "owner")
        ("priority", 100.0)
        ("filters", folly::dynamic::object
          ("db", folly::dynamic::object
            ("whitelist", { "db12" })
          )
        ),
      nullptr
    );
  }

  NodePtr host1 = std::make_shared<Node>("host1", 0, true);
  NodePtr host2 = std::make_shared<Node>("host2", 0, true);
  NodePtr db11 = std::make_shared<Node>("db11", 1, true, host1.get());
  NodePtr db12 = std::make_shared<Node>("db12", 1, true, host1.get());
  NodePtr db21 = std::make_shared<Node>("db21", 1, true, host2.get());
  NodePtr db22 = std::make_shared<Node>("db22", 1, true, host2.get());

  const int m = std::numeric_limits<int>::max();
  ResourcesByNodeType resources = {
    { host1->id(), { 2, m } },
    { host2->id(), { 2, m } },
    { db11->id(), { m, 1 } },
    { db12->id(), { m, 1 } },
    { db21->id(), { m, 1 } },
    { db22->id(), { m, 1 } },
  };

  typedef std::vector<NodePtr> NodePtrV;
  std::vector<JobWithNodes> jobs;
  jobs.emplace_back(config.jobs["job1"], NodePtrV{ db11, db12, db21, db22 });
  jobs.emplace_back(config.jobs["job2"], NodePtrV{ db11, db12, db21, db22 });
  if (add_third_job) {
    jobs.emplace_back(config.jobs["job3"], NodePtrV{ db12 });
  }
  TaskCatcher catcher;
  catcher.numTasks = Scheduler().schedule(
    jobs,
    resources,
    std::ref(catcher)
  );
  return catcher;
}

}}
