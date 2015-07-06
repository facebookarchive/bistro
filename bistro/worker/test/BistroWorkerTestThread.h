#pragma once

#include <thrift/lib/cpp/util/ScopedServerThread.h>

#include "bistro/bistro/remote/RemoteWorkerState.h"
#include "bistro/bistro/utils/TemporaryFile.h"

namespace facebook { namespace bistro {

namespace cpp2 {
  class BistroWorkerAsyncClient;
}

class ThriftMonitorTestThread;
class BistroWorkerHandler;

/**
 * Running BistroWorker service on threads for testing
 */
class BistroWorkerTestThread {
public:
  explicit BistroWorkerTestThread(ThriftMonitorTestThread*);

  std::shared_ptr<cpp2::BistroWorkerAsyncClient> getClient();

  cpp2::RunningTask runTask(
    const std::string& job,
    const std::string& node,
    const std::vector<std::string>& cmd
  );

  RemoteWorkerState::State getState() const;
  cpp2::BistroWorker getWorker() const;
  cpp2::BistroInstanceID getSchedulerID() const;

private:
  apache::thrift::util::ScopedServerThread sst_;
  std::shared_ptr<BistroWorkerHandler> workerPtr_;

  TemporaryDir dataDir_;
};

}}
