/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <thrift/lib/cpp2/util/ScopedServerThread.h>

namespace folly {
  class EventBase;
}

namespace apache::thrift {
template <class>
class Client;
} // namespace apache::thrift

namespace facebook { namespace bistro {

namespace cpp2 {
  class BistroScheduler;
  class ServiceAddress;
}

class ThriftMonitor;

/**
 * Running BistroScheduler service on threads for testing
 */
class ThriftMonitorTestThread {
public:
  ThriftMonitorTestThread();

  std::shared_ptr<apache::thrift::Client<cpp2::BistroScheduler>> getClient(
    folly::EventBase* event_base
  );

  cpp2::ServiceAddress getAddress() const;

private:
  apache::thrift::util::ScopedServerThread sst_;
  std::shared_ptr<ThriftMonitor> monitorPtr_;
};

}}
