/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/worker/test/BistroWorkerTestThread.h"

#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>

#include "bistro/bistro/server/test/ThriftMonitorTestThread.h"
#include "bistro/bistro/worker/BistroWorkerHandler.h"
#include "bistro/bistro/utils/server_socket.h"
#include "bistro/bistro/utils/service_clients.h"
#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"

DECLARE_string(data_dir);

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;
using namespace apache::thrift;

BistroWorkerTestThread::BistroWorkerTestThread(
  ThriftMonitorTestThread* scheduler) {
  // TODO 5437837 uses separate directories so we can start multiple workers
  FLAGS_data_dir = dataDir_.getPath().native();

  auto socket_and_addr = getServerSocketAndAddress();
  workerPtr_ = make_shared<BistroWorkerHandler>(
    bind(
      &ThriftMonitorTestThread::getClient,
      scheduler,
      std::placeholders::_1
    ),
    "",
    socket_and_addr.second,
    socket_and_addr.second.port
  );

  auto ts = make_shared<ThriftServer>();
  ts->setInterface(workerPtr_);
  ts->useExistingSocket(std::move(socket_and_addr.first));
  sst_.start(std::move(ts));
}

shared_ptr<cpp2::BistroWorkerAsyncClient> BistroWorkerTestThread::getClient() {
  return make_shared<cpp2::BistroWorkerAsyncClient>(
    HeaderClientChannel::newChannel(
      async::TAsyncSocket::newSocket(
        EventBaseManager::get()->getEventBase(),
        *sst_.getAddress()
      )
    )
  );
}

cpp2::RunningTask BistroWorkerTestThread::runTask(
  const string& job,
  const string& node,
  const vector<string>& cmd
) {
  cpp2::RunningTask rt;
  rt.job = job;
  rt.node = node;
  rt.workerShard = getWorker().shard;
  getClient()->sync_runTask(
    rt,
    "",
    cmd,
    getSchedulerID(),
    getWorker().id,
    0
  );
  return rt;
}

RemoteWorkerState::State BistroWorkerTestThread::getState() const {
  return workerPtr_->getState().state_;
}

cpp2::BistroWorker BistroWorkerTestThread::getWorker() const {
  return workerPtr_->getWorker();
}

cpp2::BistroInstanceID BistroWorkerTestThread::getSchedulerID() const {
  return workerPtr_->getSchedulerID();
}

}}
