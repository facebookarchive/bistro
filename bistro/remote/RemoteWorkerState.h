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

#include "bistro/bistro/if/gen-cpp2/common_types.h"

#ifndef INT64_MIN
# define INT64_MIN std::numeric_limits<int64_t>::min()
#endif

DECLARE_int32(worker_check_interval);
DECLARE_int32(heartbeat_grace_period);
DECLARE_int32(healthcheck_period);
DECLARE_int32(healthcheck_grace_period);
DECLARE_int32(lose_unhealthy_worker_after);

namespace facebook { namespace bistro {

/**
 * Some basic state that's used to determine worker health by both by the
 * scheduler and by the worker itself.
 */
struct RemoteWorkerState {
  // NEW -> UNHEALTHY <-> HEALTHY
  //            |
  //            v
  //         MUST_DIE
  // The worker is NEW until the scheduler gets its running tasks.  This
  // state also doubles as a sentinel that makes the scheduler send special
  // "new worker" health-checks (see BistroWorkerHandler::runTask).
  enum class State { NEW, HEALTHY, UNHEALTHY, MUST_DIE };

  // Can the scheduler send work here? Should the worker commit suicide?
  State state_;

  // RemoteWorker and in BistroWorkerHandler have similar update paths for
  // the values below.  It's too hard to unify the update code.

  int64_t timeBecameUnhealthy_;  // For --lose_unhealthy_worker_after
  int64_t timeLastGoodHealthcheckSent_;  // For --healthcheck_grace_period
  int64_t timeLastHeartbeatReceived_;  // For --heartbeat_grace_period

  // Used to resolve races between runTask and notifyIfTasksNotRunning
  int64_t notifyIfTasksNotRunningSequenceNum_;

  /**
   * A heartbeat for a new worker just came in. Initialize the state to
   * 'just became unhealthy, needs a healthcheck'.
   */
  explicit RemoteWorkerState(int64_t cur_time)
      // computeState() will examine this initial setup for a new worker, so
      // the per-line comments document its intended effect.
    : state_(State::NEW),
      // Avoid MUST_DIE for --lose_unhealthy_worker_after seconds
      timeBecameUnhealthy_(cur_time),
      timeLastGoodHealthcheckSent_(INT64_MIN),  // Unhealthy
      timeLastHeartbeatReceived_(INT64_MIN),
      notifyIfTasksNotRunningSequenceNum_(0) {
  }

  //
  // This is called both by scheduler and worker code to determine when a
  // worker becomes unhealthy / lost.  The worker runs this algorithm so
  // that it knows when it's about to get lost, e.g. in case of a network
  // partition.  It can therefore commit suicide at the right time, without
  // a command from the scheduler.  This gives us a pretty good guarantee
  // that we will not start duplicate tasks even during a network partition.
  //

  State computeState(
    int64_t cur_time,
    int32_t max_healthcheck_gap,
    int32_t max_heartbeat_gap,
    int32_t lose_unhealthy_worker_after
  ) const {
    if (state_ == State::MUST_DIE) {
      return State::MUST_DIE; // Can never leave this state
    }
    State new_state = State::HEALTHY;
    // The ways to leave the NEW state are: (i) go to MUST_DIE after
    // lose_unhealthy_worker_after seconds, or (ii) via
    // RemoteWorker::initializeRunningTasks or BistroWorkerHandler::heartbeat
    if (state_ == State::NEW) {
      new_state = State::NEW;
    } else if (
      (cur_time > timeLastGoodHealthcheckSent_ + max_healthcheck_gap) ||
      (cur_time > timeLastHeartbeatReceived_ + max_heartbeat_gap)
    ) {
      new_state = State::UNHEALTHY;
    }
    if (
      lose_unhealthy_worker_after > 0 &&
      // Without this check, we'd use a stale timeBecameUnhealthy_ when
      // changing from HEALTHY to UNHEALTHY.  Using != matches NEW.
      new_state != State::HEALTHY && state_ != State::HEALTHY &&
      // For NEW workers, the timeout begins at initialization time.
      cur_time > timeBecameUnhealthy_ + lose_unhealthy_worker_after
      // Don't need to add FLAGS_worker_check_interval because a worker
      // always takes at least that long to go from UNHEALTHY to MUST_DIE.
    ) {
      return State::MUST_DIE;
    }
    return new_state;
  }

  //
  // The scheduler passes these values into computeState(), and also sends
  // them to the worker so that it too can run computeState().
  //

  static int32_t maxHealthcheckGap() {
    return
      std::max(1, FLAGS_healthcheck_period) +
      std::max(1, FLAGS_healthcheck_grace_period) +
      // We can be late by this much in sending healthchecks, so be tolerant
      workerCheckInterval();
  }

  static int32_t heartbeatGracePeriod() {
    return std::max(1, FLAGS_heartbeat_grace_period);
  }

  static int32_t loseUnhealthyWorkerAfter() {
    return std::max(1, FLAGS_lose_unhealthy_worker_after);
  }

  static int32_t workerCheckInterval() {
    return std::max(1, FLAGS_worker_check_interval);
  }

  // Prepares the above worker health parameters to be sent to the worker.
  // The caller must remember to populate the .id field appropriately.
  cpp2::SchedulerHeartbeatResponse getHeartbeatResponse() {
    cpp2::SchedulerHeartbeatResponse r;
    // .id will be set by the caller
    r.maxHealthcheckGap = maxHealthcheckGap();
    r.heartbeatGracePeriod = heartbeatGracePeriod();
    r.loseUnhealthyWorkerAfter = loseUnhealthyWorkerAfter();
    r.workerCheckInterval = workerCheckInterval();
    // Tells the worker when the scheduler moved it from NEW to HEALTHY.
    r.workerState = static_cast<int32_t>(state_);
    return r;
  }

};

}}
