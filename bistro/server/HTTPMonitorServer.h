/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <proxygen/httpserver/HTTPServer.h>
#include <thread>
#include "bistro/bistro/server/HTTPMonitor.h"

DECLARE_int32(http_server_port);

namespace facebook { namespace bistro {

class HTTPMonitorServer {

public:
  explicit HTTPMonitorServer(
    std::shared_ptr<HTTPMonitor> monitor
  );

  ~HTTPMonitorServer();

  HTTPMonitorServer(const HTTPMonitorServer&) = delete;
  HTTPMonitorServer(HTTPMonitorServer&&) = delete;
  HTTPMonitorServer& operator=(const HTTPMonitorServer&) = delete;
  HTTPMonitorServer& operator=(HTTPMonitorServer&&) = delete;

private:
  std::shared_ptr<HTTPMonitor> monitor_;
  proxygen::HTTPServer server_;
  std::thread serverThread_;
};

}}
