/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>

// The default UI timeout is 7000ms, so these should sum to a lower value.
DEFINE_int32(thrift_connect_timeout_ms, 1000, "ms to make connection");
DEFINE_int32(thrift_send_timeout_ms, 1000, "ms to send request");
DEFINE_int32(thrift_receive_timeout_ms, 2000, "ms to receive response");
