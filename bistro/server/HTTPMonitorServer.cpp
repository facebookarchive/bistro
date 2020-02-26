/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/server/HTTPMonitorServer.h"
#include <folly/MPMCQueue.h>
#include <folly/Optional.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <wangle/ssl/SSLContextConfig.h>

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

  void onRequest(
      std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override {
    isSecure_ = headers->isSecure();
  }

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
      response = monitor_->handleRequest(request, isSecure_);
    } catch (const std::exception& e) {
      response = e.what();
    }
    proxygen::ResponseBuilder(downstream_)
      .status(200, "OK")
      .body(response)
      .sendWithEOM();
  }

  // We don't support upgrading the connection.
  void onUpgrade(proxygen::UpgradeProtocol /*proto*/) noexcept override {}

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
  bool isSecure_{false};
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

folly::Optional<wangle::SSLContextConfig> createSslConfig() {
  auto certPath = getenv("THRIFT_TLS_CL_CERT_PATH");
  auto keyPath = getenv("THRIFT_TLS_CL_KEY_PATH");
  auto caPath = getenv("THRIFT_TLS_SRV_CA_PATH");
  if (!(certPath && keyPath && caPath)) {
    LOG(ERROR) << "Not enabling HTTPS support because one of the environment "
      "variables THRIFT_TLS_CL_CERT_PATH, THRIFT_TLS_CL_KEY_PATH, or "
      "THRIFT_TLS_SRV_CA_PATH was not set";
    return folly::none;
  }

  wangle::SSLContextConfig config;
  config.isDefault = true;  // At least 1 config must be "default"
  // Not setting sessionContext since that doesn't seem useful.
  config.setNextProtocols(std::list<std::string>{"h2", "http/1.1"});
  config.sslVersion = SSLContext::SSLVersion::TLSv1_2;
  // Since we don't currently rely on TLS for auth, it's fine if the client
  // does not present a certificate.
  config.clientVerification = SSLContext::SSLVerifyPeerEnum::VERIFY;
  config.setCertificate(certPath, keyPath, "");  // empty passwordPath
  config.clientCAFile = caPath;
  config.eccCurveName = "prime256v1";
  return config;
}

}  // anon namespace

HTTPMonitorServer::HTTPMonitorServer(
    shared_ptr<HTTPMonitor> monitor)
  : monitor_(monitor),
    server_(buildHTTPServerOptions(monitor_.get())) {

  std::vector<proxygen::HTTPServer::IPConfig> ips = {{
    SocketAddress(
      FLAGS_http_server_address,
      FLAGS_http_server_port,
      true
    ),
    proxygen::HTTPServer::Protocol::HTTP
  }};

  // Set up TLS to allow HTTPS connections
  auto sslConfig = createSslConfig();
  if (sslConfig.has_value()) {
    LOG(INFO) << "Enabling HTTPS support";
    for (auto& ip : ips) {
      ip.sslConfigs.push_back(*sslConfig);
      // T40561409: Remove this once the clients all speak HTTPS
      ip.allowInsecureConnectionsOnSecureServer = true;
    }
  }

  server_.bind(ips);
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
