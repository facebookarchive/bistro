/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>
#include <folly/experimental/ThreadedRepeatingFunctionRunner.h>
#include <folly/init/Init.h>
#include <folly/ScopeGuard.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "bistro/bistro/Bistro.h"
#include "bistro/bistro/config/FileConfigLoader.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/runners/NoOpRunner.h"
#include "bistro/bistro/runners/BenchmarkRunner.h"
#include "bistro/bistro/runners/LocalRunner.h"
#include "bistro/bistro/runners/RemoteWorkerRunner.h"
#include "bistro/bistro/scheduler/SchedulerPolicies.h"
#include "bistro/bistro/server/HTTPMonitorServer.h"
#include "bistro/bistro/server/ThriftMonitor.h"
#include "bistro/bistro/statuses/SQLiteTaskStore.h"
#include "bistro/bistro/statuses/TaskStatuses.h"

DECLARE_int32(server_port); // from server_socket.cpp

DEFINE_string(config_file, "", "File to use for resource and job config");
DEFINE_int32(config_update_ms, 10000, "How often to re-read the config");
DEFINE_int32(nodes_update_ms, 300000, "How often to re-read the nodes");
DEFINE_int32(
  nodes_retry_ms, 30000,
  "How often to re-try reading the nodes after failures"
);
DEFINE_bool(clean_statuses, false, "If true, don't read/write statuses");
DEFINE_string(status_table, "", "Name of DB table that stores statuses.");
DEFINE_bool(
  dry_run,
  false,
  "If true, don't actually run any tasks. Also implies clean_statuses"
);
DEFINE_bool(
  benchmark_run,
  false,
  "Runs sleep tasks with configurable duration and failure, Also implies"
  "clean_statuses"
);
DEFINE_string(worker_command, "", "If set, use local task mode");
DEFINE_string(data_dir, "/data/bistro", "Data for local tasks is stored here");

using namespace std;
using namespace facebook::bistro;

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = 1;
  folly::init(&argc, &argv);

  registerDefaultSchedulerPolicies();

  boost::filesystem::path config_file(FLAGS_config_file);
  auto config_loader = make_shared<FileConfigLoader>(
    std::chrono::milliseconds(FLAGS_config_update_ms),
    boost::filesystem::absolute(config_file)
  );
  auto nodes_loader = make_shared<NodesLoader>(
    config_loader,
    std::chrono::milliseconds(FLAGS_nodes_update_ms),
    std::chrono::milliseconds(FLAGS_nodes_retry_ms)
  );

  shared_ptr<TaskStore> task_store;
  if (FLAGS_clean_statuses || FLAGS_dry_run || FLAGS_benchmark_run) {
    task_store.reset(new NoOpTaskStore());
  } else {
    CHECK(!FLAGS_status_table.empty()) << "You must pass --status_table";
    task_store.reset(new SQLiteTaskStore(FLAGS_data_dir, FLAGS_status_table));
  }

  auto task_statuses = make_shared<TaskStatuses>(task_store);

  auto monitor = make_shared<Monitor>(
    config_loader,
    nodes_loader,
    task_statuses
  );

  shared_ptr<TaskRunner> task_runner;
  if (FLAGS_dry_run) {
    task_runner.reset(new NoOpRunner());
  } else if (FLAGS_benchmark_run) {
    task_runner.reset(new BenchmarkRunner());
  } else if (!FLAGS_worker_command.empty()) {
    task_runner.reset(new LocalRunner(FLAGS_worker_command, FLAGS_data_dir));
  } else {
    task_runner.reset(new RemoteWorkerRunner(task_statuses, monitor));
  }

  // Initialize the scheduler thread itself
  Bistro bistro(
    config_loader,
    nodes_loader,
    task_statuses,
    task_runner,
    monitor
  );
  folly::ThreadedRepeatingFunctionRunner bistro_thread;
  bistro_thread.add(
    "BistroSchedule", bind(&Bistro::scheduleOnceSystemTime, &bistro)
  );
  SCOPE_EXIT { bistro_thread.stop(); };

  auto http_monitor = make_shared<HTTPMonitor>(
    config_loader,
    nodes_loader,
    task_statuses,
    task_runner,
    monitor
  );

  HTTPMonitorServer http_monitor_server(http_monitor);

  // Initialize the thrift monitor
  auto handler = std::make_unique<ThriftMonitor>(
    config_loader,
    nodes_loader,
    task_statuses,
    task_runner,
    monitor
  );

  auto server = make_shared<apache::thrift::ThriftServer>();
  server->setPort(FLAGS_server_port);
  server->setInterface(std::move(handler));
  server->serve();
}
