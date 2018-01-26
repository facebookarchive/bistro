/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gflags/gflags.h>

// The default UI timeout is 7000ms, so these should sum to a lower value.
DEFINE_int32(thrift_connect_timeout_ms, 1000, "ms to make connection");
DEFINE_int32(thrift_send_timeout_ms, 1000, "ms to send request");
DEFINE_int32(thrift_receive_timeout_ms, 2000, "ms to receive response");
