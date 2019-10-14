/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Memory.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <memory>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"
#include "bistro/bistro/utils/service_clients.h"
#include "bistro/bistro/utils/server_socket.h"
#include "bistro/bistro/worker/BistroWorkerHandler.h"
#include "bistro/bistro/worker/StopWorkerOnSignal.h"

// TODO: It would be useful to periodically re-read this host:port from a
// file to ensure your scheduler can survive machine failures.
DEFINE_string(scheduler_host, "", "Scheduler's hostname.");
DEFINE_int32(scheduler_port, 0, "Scheduler's thrift port.");
DEFINE_string(worker_command, "", "Command to run for the worker.");
DEFINE_string(
  data_dir, "/data/bistro", "Where to create status pipes and job directories"
);

static const bool scheduler_host_validator = gflags::RegisterFlagValidator(
    &FLAGS_scheduler_host,
    [](const char* /*flagname*/, const std::string& value) {
      return !value.empty();
    });

static const bool scheduler_port_validator = gflags::RegisterFlagValidator(
    &FLAGS_scheduler_port,
    [](const char* /*flagname*/, int32_t value) {
      return value > 0 && value < 65536;
    });

static const bool worker_command_validator = gflags::RegisterFlagValidator(
    &FLAGS_worker_command,
    [](const char* /*flagname*/, const std::string& value) {
      return !value.empty();
    });

using namespace facebook::bistro;

int main(int argc, char* argv[]) {
  FLAGS_logtostderr = 1;
  folly::init(&argc, &argv);

  cpp2::ServiceAddress scheduler_addr;
  // DO: It would be faster & more robust to pre-resolve the hostname here.
  scheduler_addr.ip_or_host = FLAGS_scheduler_host;
  scheduler_addr.port = FLAGS_scheduler_port;

  auto my_socket_and_addr = getServerSocketAndAddress();
  auto server = std::make_shared<apache::thrift::ThriftServer>();
  auto handler = std::make_shared<BistroWorkerHandler>(
    server,  // The handler calls server->stop() on suicide.
    FLAGS_data_dir,
    [](const char*, const cpp2::BistroWorker&, const cpp2::RunningTask*) {
      // Do not log state transitions. This would be a good place to hook up
      // a popular OSS tool for collecting operational charts.
    },
    [scheduler_addr](folly::EventBase* event_base) {
      // Future: add plugins to poll various discovery mechanisms here.
      return getAsyncClientForAddress<cpp2::BistroSchedulerAsyncClient>(
        event_base,
        scheduler_addr
      );
    },
    FLAGS_worker_command,
    my_socket_and_addr.second,  // Could change in the presence of proxies
    my_socket_and_addr.second.port  // Actual local port the worker has locked
  );
  StopWorkerOnSignal signal_handler(
    folly::EventBaseManager::get()->getEventBase(),
    {SIGTERM, SIGINT, SIGQUIT, SIGHUP},
    handler
  );

  server->useExistingSocket(std::move(my_socket_and_addr.first));
  server->setInterface(std::move(handler));
  server->serve();
  return 1;  // Exit means we got a signal, suicide request, etc.
}
