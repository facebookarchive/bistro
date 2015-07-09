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

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <functional>
#include <memory>

namespace facebook { namespace bistro {

class Connection;
typedef std::shared_ptr<Connection> ConnectionPtr;

typedef std::function<std::string(const std::string&)> RequestProcessor;

class Connection : public std::enable_shared_from_this<Connection> {

public:
  Connection(
    boost::asio::ip::tcp::socket socket,
    std::function<void(ConnectionPtr)> stop_callback,
    RequestProcessor process_callback
  );

  ~Connection();

  void start();

  void stop();

private:
  boost::asio::ip::tcp::socket socket_;
  std::function<void(ConnectionPtr)> stopCallback_;
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
