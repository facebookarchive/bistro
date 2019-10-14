/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    function<void(Connection*)> destroy_callback,
    RequestProcessor process_callback)
  : socket_(std::move(socket)),
    destroyCallback_(destroy_callback),
    processCallback_(process_callback),
    contentLength_(-1) {
}

Connection::~Connection() {}

void Connection::start() {
  read();
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
          // Calls destroyCallback_ later, so `this` may become invalid
          write();
        } else {
          // This same read handler will run again, `this` may become invalid
          read();
        }
        // `this` might not exist here any more.
      } else {
        LOG(ERROR) << "Error reading request: " << ec;
        socket_.close();
        destroyCallback_(this);
        // DANGER: `this` has now been destroyed.
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
      [this](boost::system::error_code ec, size_t /*bytes_transferred*/) {
        if (ec) {
          LOG(ERROR) << "Error writing request: " << ec;
        }
        socket_.close();
        destroyCallback_(this);
        // DANGER: `this` has now been destroyed.
      });
}

}}
