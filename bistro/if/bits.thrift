/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */

namespace cpp facebook.bistro
namespace py facebook.bistro.bits

enum BistroTaskStatusBits {
  UNSTARTED = 1

  RUNNING = 2
  DONE = 4
  INCOMPLETE = 8
  FAILED = 16
  ERROR = 32

  USES_BACKOFF = 64
  DOES_NOT_ADVANCE_BACKOFF = 512

  OVERWRITEABLE = 1024

  // Private, cannot be reported by tasks. The UI can peek at this as a
  // hacky proxy for determining whether a task had an error or the worker
  // was lost, but do NOT rely on this behavior, it may change.
  HAS_SAVED_BACKOFF = 2048

  CURRENT_STATUS_MASK = 3711  // OR of the preceding bits

  AVOIDED = 128
  DISABLED = 256

  CAN_RUN_MASK = 384  // OR of the preceding bits
}
