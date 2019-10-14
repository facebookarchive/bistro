/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <folly/Benchmark.h>

#include "bistro/bistro/Bistro.h"
#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/InMemoryConfigLoader.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/runners/BenchmarkRunner.h"

DEFINE_int32(num_hosts, 12000, "Number of hosts");
DEFINE_int32(num_shards_per_host, 25, "Number of shards per host");
DEFINE_int32(num_jobs, 1, "Number of Bistro jobs");

DECLARE_int32(log_status_changes_every_ms);

using namespace std;
using namespace folly;

using namespace facebook::bistro;

dynamic c = dynamic::object
  ("nodes", dynamic::object
    ("levels", dynamic::array("host", "db"))
    ("node_sources", dynamic::array(
      dynamic::object
        ("prefs", dynamic::object
          ("parent_level", "instance")
          ("format", "host{i}")
          ("start", 1)
          ("end", FLAGS_num_hosts)
        )
        ("source", "range_label"),
      dynamic::object
        ("prefs", dynamic::object
          ("parent_level", "host")
          ("format", "{parent}.{i}")
          ("start", 1)
          ("end", FLAGS_num_shards_per_host)
        )
        ("source", "range_label")
    ))
  )
  ("resources", dynamic::object
    ("host", dynamic::object
      ("host_concurrency", dynamic::object("default", 1)("limit", 2))
    )
    ("db", dynamic::object
      ("db_concurrency", dynamic::object("default", 1)("limit", 1))
    )
  )
  ("enabled", true)
;

struct TaskCounter : public TaskStore {
  int count_ = 0;
  void fetchJobTasks(const vector<string>& /*job_ids*/, Callback /*cb*/)
      override {}
  void store(
      const string& /*job*/,
      const string& /*node*/,
      TaskResult /*result*/) override {
    ++count_;
  }
};

BENCHMARK(MakeSpan) {
  Config config(c);
  for (int i=1; i <= FLAGS_num_jobs; i++) {
    config.addJob(
        std::make_shared<Job>(
            config,
            to<string>("job", i),
            dynamic::object("owner", "owner")),
        nullptr
    );
  }
  auto config_loader = make_shared<InMemoryConfigLoader>(config);
  auto nodes_loader = make_shared<NodesLoader>(
    config_loader,
    std::chrono::seconds(3600),
    std::chrono::seconds(3600)
  );
  auto task_counter = make_shared<TaskCounter>();
  auto task_statuses = make_shared<TaskStatuses>(task_counter);
  auto task_runner = make_shared<BenchmarkRunner>();
  auto monitor = make_shared<Monitor>(
    config_loader,
    nodes_loader,
    task_statuses
  );

  Bistro bistro(
    config_loader,
    nodes_loader,
    task_statuses,
    task_runner,
    monitor
  );

  // Terminate half way to avoid long tails.  In addition, the throughput
  // always goes up as more and more tasks finish, and then drops when
  // there is not enough tasks to keep all resources busy.  Cutting the
  // benchmark half way measures heavy workload.
  auto max_tasks =
    FLAGS_num_hosts * FLAGS_num_shards_per_host * FLAGS_num_jobs / 2;
  while (task_counter->count_ <= max_tasks) {
    bistro.scheduleOnceSystemTime();
  }
}

int main(int argc, char** argv) {
  FLAGS_log_status_changes_every_ms = 1000000;
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  runBenchmarks();
  return 0;
}
