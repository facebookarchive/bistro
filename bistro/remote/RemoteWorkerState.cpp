/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
  lose_unhealthy_worker_after, 500,
  "If a remote worker is unhealthy for this many seconds, consider it lost. "
  "This means we decide that its tasks failed, and start to send it suicide "
  "commands. On startup, a scheduler waits for lose_unhealthy_worker_after + "
  "healthcheck_period + healthcheck_grace_period + worker_check_interval + "
  "lose_unhealthy_worker_after + worker_suicide_* to ensure all live workers "
  "have connected before running tasks. This flag should low enough to keep "
  "this wait tolerable but high enough that heavy IO on workers does not "
  "cause them to be lost. In principle, a value over 2*healthcheck_period + "
  "4*heartbeat_period + 2*worker_check_interval should suffice, but in "
  "practice a higher safety factor reduces the frequency of restarts of a "
  "crash-looping worker, which in turn makes it less disruptive to reaching "
  "a worker-set consensus among the other workers -- see the note on worker "
  "turnover in README.worker_set_consensus. Values below 1 get bumped to 1."
);
DEFINE_int32(worker_check_interval, 5,
  "How often to check if a worker is due for a healthcheck, became unhealthy "
  "or lost, etc. Keep this low to keep worker state up-to-date. Checks have "
  "a cost for each worker, so large deployments will need to increase this. "
  "Values below 1 get bumped to 1."
);
