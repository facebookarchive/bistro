/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/server/test/ThriftMonitorTestThread.h"

#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>

#include "bistro/bistro/config/InMemoryConfigLoader.h"
#include "bistro/bistro/statuses/TaskStore.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/monitor/Monitor.h"
#include "bistro/bistro/runners/RemoteWorkerRunner.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/server/ThriftMonitor.h"


namespace facebook { namespace bistro {

using namespace folly;
using namespace std;
using namespace apache::thrift;

namespace {
const dynamic c = dynamic::object
  ("nodes", dynamic::object
    ("levels", dynamic::array("level1", "level2"))
    ("node_sources", dynamic::array(dynamic::object
      ("source", "manual")
      ("prefs", dynamic::object
        ("node1", dynamic::array("node11", "node12"))
        ("node2", dynamic::array("node21", "node22"))
      )
    ))
  )
  ("resources", dynamic::object)
  ("enabled", true)
;
}

ThriftMonitorTestThread::ThriftMonitorTestThread() {
  // try to match main.cpp, otherwise the test can be irrelevant
  Config config(c);
  auto config_loader = make_shared<InMemoryConfigLoader>(config);
  auto nodes_loader = make_shared<NodesLoader>(
    config_loader,
    std::chrono::hours(1),
    std::chrono::hours(1)
  );
  auto done_store = make_shared<NoOpTaskStore>();
  auto task_statuses = make_shared<TaskStatuses>(done_store);
  auto monitor = make_shared<Monitor>(
    config_loader,
    nodes_loader,
    task_statuses
  );
  auto task_runner = make_shared<RemoteWorkerRunner>(task_statuses, monitor);

  auto ts = make_shared<ThriftServer>();
  ts->setAddress("::1", 0); // localhost IPv6, random unused port
  monitorPtr_ = make_shared<ThriftMonitor>(
    config_loader,
    nodes_loader,
    task_statuses,
    task_runner,
    monitor
  );
  ts->setInterface(monitorPtr_);
  sst_.start(std::move(ts));
}

shared_ptr<cpp2::BistroSchedulerAsyncClient>
ThriftMonitorTestThread::getClient(folly::EventBase* event_base) {
  return make_shared<cpp2::BistroSchedulerAsyncClient>(
      HeaderClientChannel::newChannel(
          async::TAsyncSocket::newSocket(event_base, *sst_.getAddress())
      )
  );
}

cpp2::ServiceAddress ThriftMonitorTestThread::getAddress() const {
  cpp2::ServiceAddress addr;
  auto fs = sst_.getAddress();
  addr.ip_or_host = fs->getAddressStr();
  addr.port = fs->getPort();
  return addr;
}

}}
