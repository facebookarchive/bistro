/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncServerSocket.h>

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

/**
 * Makes a socket, on which this server will listen, and returns the socket
 * with its address.  Unless overridden via --server_* command-line
 * arguments, automatically chooses:
 *  - the network interface
 *  - the address & port
 * Also see ServiceClients::getAsyncClientForAddress.
 *
 * This is a good place to add proxy support.
 */
std::pair<folly::AsyncServerSocket::UniquePtr, cpp2::ServiceAddress>
  getServerSocketAndAddress();

}}
