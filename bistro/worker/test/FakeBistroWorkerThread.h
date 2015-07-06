#pragma once

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>
#include "bistro/bistro/if/gen-cpp2/BistroWorker.h"
#include "bistro/bistro/utils/hostname.h"

namespace facebook { namespace bistro {

class FakeBistroWorker : public virtual cpp2::BistroWorkerSvIf {

public:
 ~FakeBistroWorker() override {}

 void async_tm_runTask(
     std::unique_ptr<apache::thrift::HandlerCallback<void>> cb,
     const cpp2::RunningTask& rt,
     const std::string& config,
     const std::vector<std::string>& command,
     const cpp2::BistroInstanceID& scheduler,
     const cpp2::BistroInstanceID& worker,
     int64_t notify_if_tasks_not_running_sequence_num) override;

 void async_tm_getRunningTasks(
     std::unique_ptr<
         apache::thrift::HandlerCallback<std::vector<cpp2::RunningTask>>> cb,
     const cpp2::BistroInstanceID& worker) override;
};

class FakeBistroWorkerThread {

public:
  explicit FakeBistroWorkerThread(std::string shard = getLocalHostName()) :
    shard_(std::move(shard)),
    ssit_(std::make_shared<FakeBistroWorker>()) {
  }

  cpp2::BistroWorker getBistroWorker();

private:
  std::string shard_;
  apache::thrift::ScopedServerInterfaceThread ssit_;
};

}}
