/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#define BOOST_ASIO_HAS_MOVE 1

#include "bistro/bistro/server/HTTPServer.h"

#include <iostream>

#include <folly/Conv.h>

namespace facebook { namespace bistro {

using namespace std;

HTTPServer::HTTPServer(
  int port,
  const string& address,
  RequestProcessor process_callback)
  : processCallback_(process_callback),
    ioService_(),
    acceptor_(ioService_),
    socket_(ioService_) {

  boost::asio::ip::tcp::resolver resolver(ioService_);
  boost::asio::ip::tcp::endpoint endpoint =
    *resolver.resolve({address, folly::to<string>(port),
        boost::asio::ip::resolver_query_base::numeric_service});

  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();
  accept();
}

void HTTPServer::run() {
  ioService_.run();
}

void HTTPServer::accept() {
  acceptor_.async_accept(socket_, [this](boost::system::error_code ec) {
    if (!acceptor_.is_open()) {
      return;
    }
    if (!ec) {
      auto c = make_shared<Connection>(
        // TODO(agoder): This move seems dubious, or at least hard-to-follow.
        std::move(socket_),
        [this](ConnectionPtr p) {
          connections_.erase(p);
          p->stop();
        },
        processCallback_
      );
      connections_.insert(c);
      c->start();
    }
    accept();
  });
}

}}
