/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/server/HTTPMonitorServer.h"
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <folly/MPMCQueue.h>

DEFINE_int32(http_server_port, 8080, "Port to run HTTP server on");
DEFINE_string(http_server_address, "::", "Address to bind to");
DEFINE_int32(http_server_threads, 1, "Number of threads for http server");

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {

class BistroHTTPHandler : public proxygen::RequestHandler {
public:
  explicit BistroHTTPHandler(HTTPMonitor* monitor)
    : monitor_(monitor) {}

  void onRequest(std::unique_ptr<proxygen::HTTPMessage> headers)
    noexcept override {}

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override {
    if (body_) {
      body_->prependChain(std::move(body));
    } else {
      body_ = std::move(body);
    }
  }

  void onEOM() noexcept override {
    auto request = body_ ? body_->moveToFbString() : fbstring();
    fbstring response;
    try {
      response = monitor_->handleRequest(request);
    } catch (const std::exception& e) {
      response = e.what();
    }
    proxygen::ResponseBuilder(downstream_)
      .status(200, "OK")
      .body(response)
      .sendWithEOM();
  }

  // We don't support upgrading the connection.
  void onUpgrade(proxygen::UpgradeProtocol proto) noexcept override {}

  void requestComplete() noexcept override {
    delete this;
  }

  void onError(proxygen::ProxygenError err) noexcept override {
    LOG(ERROR) << "Error in HTTP Handler: " << proxygen::getErrorString(err);
    delete this;
  }

private:
  HTTPMonitor* monitor_;
  std::unique_ptr<folly::IOBuf> body_;
};

class BistroHTTPHandlerFactory : public proxygen::RequestHandlerFactory {
public:
  explicit BistroHTTPHandlerFactory(HTTPMonitor* monitor)
    : monitor_(monitor) {}

  void onServerStart(folly::EventBase*) noexcept override {}
  void onServerStop() noexcept override {}

  proxygen::RequestHandler* onRequest(
    proxygen::RequestHandler*,
    proxygen::HTTPMessage*
  ) noexcept override {
    return new BistroHTTPHandler(monitor_);
  }

private:
  HTTPMonitor* monitor_;
};

proxygen::HTTPServerOptions buildHTTPServerOptions(HTTPMonitor* monitor) {
  proxygen::HTTPServerOptions options;
  options.threads = static_cast<size_t>(FLAGS_http_server_threads);
  options.enableContentCompression = true;
  options.handlerFactories = proxygen::RequestHandlerChain()
    .addThen<BistroHTTPHandlerFactory>(monitor)
    .build();
  return options;
}
}

HTTPMonitorServer::HTTPMonitorServer(
    shared_ptr<HTTPMonitor> monitor)
  : monitor_(monitor),
    server_(buildHTTPServerOptions(monitor_.get())) {

  std::vector<proxygen::HTTPServer::IPConfig> IPs = {{
    SocketAddress(
      FLAGS_http_server_address,
      FLAGS_http_server_port,
      true
    ),
    proxygen::HTTPServer::Protocol::HTTP
  }};
  server_.bind(IPs);
  folly::MPMCQueue<int> completer(1);
  serverThread_ = std::thread([this, &completer]() {
    server_.start(
      // onSuccess
      [&completer](){ completer.blockingWrite(0); },
      // onError
      [&completer](std::exception_ptr) { completer.blockingWrite(-1); }
    );
  });

  int result;
  completer.blockingRead(result);

  LOG(INFO) << "Launched HTTP Monitor Server on port "
            << FLAGS_http_server_port
            << ", result " << result;
}

HTTPMonitorServer::~HTTPMonitorServer() {
  server_.stop();
  serverThread_.join();
}

}}
