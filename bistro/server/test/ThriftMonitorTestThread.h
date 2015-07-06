#pragma once

#include <thrift/lib/cpp/util/ScopedServerThread.h>

namespace folly {
  class EventBase;
}

namespace facebook { namespace bistro {

namespace cpp2 {
  class BistroSchedulerAsyncClient;
  class ServiceAddress;
}

class ThriftMonitor;

/**
 * Running BistroScheduler service on threads for testing
 */
class ThriftMonitorTestThread {
public:
  ThriftMonitorTestThread();

  std::shared_ptr<cpp2::BistroSchedulerAsyncClient> getClient(
    folly::EventBase* event_base
  );

  cpp2::ServiceAddress getAddress() const;

private:
  apache::thrift::util::ScopedServerThread sst_;
  std::shared_ptr<ThriftMonitor> monitorPtr_;
};

}}
