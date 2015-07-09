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
#include "bistro/bistro/server/Connection.h"

#include <glog/logging.h>

#include <folly/Conv.h>

namespace facebook { namespace bistro {

using namespace std;
using folly::to;

namespace {
  const string kContentLength = "Content-Length: ";
  const string kHeaderEnd = "\r\n";
  const string kAllHeaderEnd = "\r\n\r\n";
}

Connection::Connection(
    boost::asio::ip::tcp::socket socket,
    function<void(ConnectionPtr)> stop_callback,
    RequestProcessor process_callback)
  : socket_(std::move(socket)),
    stopCallback_(stop_callback),
    processCallback_(process_callback),
    contentLength_(-1) {
}

Connection::~Connection() {}

void Connection::start() {
  read();
}

void Connection::stop() {
  socket_.close();
}

bool Connection::parseRequest() {
  if (contentLength_ < 0) {
    // Try to figure out the content length we expect
    auto it = search(
      request_.begin(),
      request_.end(),
      kContentLength.begin(),
      kContentLength.end(),
      // Headers should be case insensitive
      [](char a, char b) {
        return tolower(a) == tolower(b);
      }
    );
    if (it == request_.end()) {
      return false;
    }
    auto jt = search(it, request_.end(), kHeaderEnd.begin(), kHeaderEnd.end());
    if (jt == request_.end()) {
      return false;
    }
    for (it = it + kContentLength.size(); isspace(*it); ++it) {
      // We could have extra whitespace before the value
    }
    contentLength_ = to<int>(string(it, jt));
  }

  // Check if we've received the full content length
  auto it = search(
    request_.begin(),
    request_.end(),
    kAllHeaderEnd.begin(),
    kAllHeaderEnd.end()
  );
  if (it == request_.end()) {
    return false;
  }
  if (contentLength_ > distance(it, request_.end())) {
    return false;
  }

  // We've received the full message, erase the headers
  request_.erase(request_.begin(), it + kAllHeaderEnd.size());
  return true;
}

void Connection::read() {
  socket_.async_read_some(
    boost::asio::buffer(buffer_),
    [this](boost::system::error_code ec, size_t bytes_transferred) {
      if (!ec) {
        request_.append(buffer_.data(), bytes_transferred);
        if (parseRequest()) {
          response_ = processCallback_(request_);
          write();
        } else {
          read();
        }
      } else if (ec != boost::asio::error::operation_aborted) {
        LOG(ERROR) << "Error reading request: " << ec;
        stopCallback_(shared_from_this());
      }
    }
  );
}

void Connection::write() {
  boost::asio::async_write(
    socket_,
    std::vector<boost::asio::const_buffer>{
      boost::asio::buffer("HTTP/1.0 200 OK"),
      boost::asio::buffer(kHeaderEnd),
      boost::asio::buffer(kContentLength),
      boost::asio::buffer(to<string>(response_.size())),
      boost::asio::buffer(kAllHeaderEnd),
      boost::asio::buffer(response_),
    },
    [this](boost::system::error_code ec, size_t bytes_transferred) {
      if (!ec) {
        boost::system::error_code ignored;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
      } else if (ec != boost::asio::error::operation_aborted) {
        stopCallback_(shared_from_this());
      }
    }
  );
}

}}
