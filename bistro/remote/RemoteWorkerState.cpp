/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/remote/RemoteWorker.h"

DEFINE_int32(
  heartbeat_grace_period, 60,
  "Each remote worker tells us their heartbeat period. If the time since "
  "the last heartbeat exceeds that period, plus the grace period in seconds, "
  "the worker is marked unhealthy. Values below 1 get bumped to 1."
);
DEFINE_int32(
  healthcheck_period, 60,
  "Every how many seconds should the scheduler send a healthcheck to each "
  "remote worker? Values below 1 get bumped to 1."
);
DEFINE_int32(
  healthcheck_grace_period, 60,
  "If the time since we **sent** the last successful healthcheck exceeds "
  "healthcheck_period + healtcheck_grace_period, we mark the remote worker "
  "unhealthy. Values below 1 get bumped to 1."
);
DEFINE_int32(
  lose_unhealthy_worker_after, 180,
  "If a remote worker is unhealthy for this many seconds, consider it lost. "
  "This means we decide that its tasks failed, and start to send it suicide "
  "commands. On startup, a scheduler waits for healthcheck_period + "
  "healthcheck_grace_period + worker_check_interval + "
  "lose_unhealthy_worker_after to ensure all live workers have connected "
  "before running tasks. This value should low enough to be tolerable but "
  "high enough that heavy worker load does not typically cause it to be lost. "
  "In particular, aim for over 2*healthcheck_period + worker_check_interval "
  "to avoid losing workers due to one missed healthcheck. Values below 1 "
  "get bumped to 1."
);
DEFINE_int32(worker_check_interval, 5,
  "How often to check if a worker is due for a healthcheck, became unhealthy "
  "or lost, etc. Keep this low to keep worker state up-to-date. Checks have "
  "a cost for each worker, so large deployments will need to increase this. "
  "Values below 1 get bumped to 1."
);
