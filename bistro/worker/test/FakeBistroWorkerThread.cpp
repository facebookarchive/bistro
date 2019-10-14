/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/worker/test/FakeBistroWorkerThread.h"

#include "bistro/bistro/if/gen-cpp2/common_constants.h"
#include "bistro/bistro/utils/hostname.h"

using namespace std;

namespace facebook { namespace bistro {

void FakeBistroWorker::async_tm_runTask(
    unique_ptr<apache::thrift::HandlerCallback<void>> cb,
    const cpp2::RunningTask& rt,
    const string& /*config*/,
    const std::vector<std::string>& /*command*/,
    const cpp2::BistroInstanceID& /*scheduler*/,
    const cpp2::BistroInstanceID& /*worker*/,
    int64_t /*notify_if_tasks_not_running_sequence_num*/,
    const cpp2::TaskSubprocessOptions& tso) {
  taskSubprocessOptsCob_(rt, tso);
  cb->done();
}

void FakeBistroWorker::async_tm_getRunningTasks(
    std::unique_ptr<
        apache::thrift::HandlerCallback<std::vector<cpp2::RunningTask>>> cb,
    const cpp2::BistroInstanceID& /*worker*/) {
  std::vector<cpp2::RunningTask> tasks;
  cb->result(tasks);
}

cpp2::BistroWorker FakeBistroWorkerThread::getBistroWorker() const {
  cpp2::BistroWorker worker;
  worker.shard = shard_;
  worker.machineLock.hostname = getLocalHostName();
  worker.machineLock.port = ssit_.getPort();
  worker.addr.ip_or_host = ssit_.getAddress().getAddressStr();
  worker.addr.port = worker.machineLock.port;
  worker.protocolVersion = cpp2::common_constants::kProtocolVersion();
  customizeWorkerCob_(&worker);
  return worker;
}

}}
