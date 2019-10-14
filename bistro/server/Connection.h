/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <functional>
#include <memory>

namespace facebook { namespace bistro {

class Connection;

typedef std::function<std::string(const std::string&)> RequestProcessor;

class Connection {
public:
  Connection(
    boost::asio::ip::tcp::socket socket,
    std::function<void(Connection*)> destroy_cob,
    RequestProcessor process_callback
  );

  ~Connection();

  void start();

private:
  boost::asio::ip::tcp::socket socket_;
  // IMPORTANT: The lifetime semantics of Connection are simple and
  // dangerous: it is owned externally, and once its job is done, calls
  // destroyCallback_ to tell its owner to destroy it.  So, it is crucial
  // never to access `this` after a call that potentially destroys `this`.
  std::function<void(Connection*)> destroyCallback_;
  RequestProcessor processCallback_;
  // this works with gcc, but not with clang:dev
  boost::array<char, 8192> buffer_;
  std::string request_;
  std::string response_;
  int contentLength_;

  void read();
  void write();
  bool parseRequest();
};

}}
