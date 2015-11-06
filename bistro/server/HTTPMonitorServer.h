/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <proxygen/httpserver/HTTPServer.h>
#include <thread>
#include "bistro/bistro/server/HTTPMonitor.h"

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
