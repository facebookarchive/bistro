/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/worker/test/FakeBistroWorkerThread.h"

using namespace std;

namespace facebook { namespace bistro {

void FakeBistroWorker::async_tm_runTask(
  unique_ptr<apache::thrift::HandlerCallback<void>> cb,
  const cpp2::RunningTask& rt,
  const string& config,
  const std::vector<std::string>& command,
  const cpp2::BistroInstanceID& scheduler,
  const cpp2::BistroInstanceID& worker,
  int64_t notify_if_tasks_not_running_sequence_num) {

  cb->done();
}

void FakeBistroWorker::async_tm_getRunningTasks(
  std::unique_ptr<apache::thrift::HandlerCallback<
    std::vector<cpp2::RunningTask>>> cb,
  const cpp2::BistroInstanceID& worker) {

  std::vector<cpp2::RunningTask> tasks;
  cb->result(tasks);
}

cpp2::BistroWorker FakeBistroWorkerThread::getBistroWorker() {
  cpp2::BistroWorker worker;
  worker.shard = shard_;
  auto name = getLocalHostName();
  int port = ssit_.getPort();
  worker.machineLock.hostname = name;
  worker.machineLock.port = port;
  worker.addr.ip_or_host = ssit_.getAddress().getAddressStr();
  worker.addr.port = port;
  return worker;
}

}}
