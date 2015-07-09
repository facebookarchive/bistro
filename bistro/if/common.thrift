/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 */

namespace cpp facebook.bistro
namespace php bistro
namespace py facebook.bistro.common

// Marks LogLines that are not authentic log lines, but which instead store
// some kind of error or warning about the log line fetching process.
const i64 kNotALineID = -1;

// Both members are required because we keep hundreds of millions of these
// in memory, and therefore don't want to waste bits on the __isset field.
struct BackoffDuration {
  // The job is out of retries, it failed permanently.
  1: required bool noMoreBackoffs,
  // Wait this long before trying again.
  2: required i32 seconds,
} (final)

struct LogLine {
  1: string jobID,
  2: string nodeID,
  3: i32 time,
  4: string line,
  5: i64 lineID,  // Use lineID to start paging from this log line.
} (final)

struct LogLines {
  // Can contain "error" lines whose lineID == kNotALineID
  1: list<LogLine> lines,
  // Contains the lineID of the next page, kNotALineID if no more lines
  2: i64 nextLineID,
} (final)

struct Resource {
  1: required string name,
  2: required i32 amount,
} (final)

struct NodeResources {
  1: required string node,
  2: required list<Resource> resources,
} (final)

// Distinguish different invocations of the same task, or scheduler or
// worker instances that use the same host:port at different points in time
// (it's assumed that a host:port pair is never shared).
struct BistroInstanceID {
  1: i64 startTime,
  2: i64 rand,
}

// This structure exists for two reasons:
//  1) Bistro must remember the tasks it had started, even if the original
//     job & node are no longer registered.  It should also persist these
//     across restarts.
//  2) The worker should have an easy and robust way of reporting back this
//     same information to Bistro (in case of e.g. restart).
struct RunningTask {
  1: string job,
  2: string node,
  // Storing the resource map accomplishes two things:
  //  - Running tasks that belong to deleted jobs or nodes still get correct
  //    resource accounting (at the cost of resources used by running tasks
  //    not getting updated implicitly when the configuration changes --
  //    arguably, a good feature anyhow).
  //  - For nodes with replicas, this implicitly tracks the correct replica.
  3: list<NodeResources> nodeResources,
  4: string workerShard,
  // Disinguish different invocations of the same task.
  5: BistroInstanceID invocationID,
  6: BackoffDuration nextBackoffDuration,  // How long to back off on error
}

// This structure isn't for incoming connections, use ServiceAddress for
// that.  Don't even try to DNS-resolve this hostname.  It's just an ID.
//
// Given a unique machine ID (e.g. a canonical hostname) and an exclusive
// resource on that machine (e.g. a port locked for listening on all
// relevant network interfaces), we can be sure that only one instance of a
// process exists with those parameters.
//
// More specifically, when the scheduler gets a new worker instance
// sure that the previous one is dead, and does not need to kill it, or do
// any other complex conflict resolution.  This means that Bistro workers
// with fixed hostnames and ports are especially robust operationally.
//
// We also use this host:port identifier to configure worker resource
// overrides, to implement task host placement constraints, and to enrich
// the logs.
//
// If your machine setup lacks reliable hostnames, fix it, or make Bistro
// return other stable machine IDs for your workers.
struct MachinePortLock {
  1: string hostname,  // Never connect here; used as a "unique machine ID"
  2: i32 port,  // Not for incoming connections; used as a "process lock"
}

// How can the scheduler connect back to a worker?  This is the place to add
// support for proxies or secure thrift.
struct ServiceAddress {
  // Not guaranteed to be canonical. Do *not* use this as a machine ID, use
  // MachinePortLock hostname instead.  Since IPs of server machines tend
  // not to change, prefer to use IPs here.  Resolving hostnames is slower
  // and makes your service vulnerable to DNS outages.
  1: string ip_or_host,
  // Which port to connect to?
  2: i32 port,  // No unsigned 16-bit integer in Thrift
}

// Describes and uniquely identifies a worker instance
//
// WARNING: If you delete or reorder fields, you must update the places in
// Bistro that currently use the FRAGILE constructor.
struct BistroWorker {
  // This identifies the worker's position in the worker pool. It's
  // important to have these because it lets us implement more robust worker
  // failover / locking.  When a new worker comes along for the same shard
  // ID, this tells us that the previous worker instance really is down, as
  // opposed to transiently unavailable.  The scheduler can then consider
  // the old worker's jobs to have failed.
  //
  // Could be a number (not necessarily sequential) or a hostname, or any
  // other convenient identifier (see README.worker_protocol).
  1: string shard,

  2: MachinePortLock machineLock,
  // Do NOT use addr to compare worker locations, use machineLock instead.
  3: ServiceAddress addr,
  // Distinguish different instances on the same host:port (MachinePortLock)
  4: BistroInstanceID id,

  // Make it unnecessary to manually set this config for the scheduler.
  5: i32 heartbeatPeriodSec,  // The scheduler adds a grace period
}

struct SchedulerHeartbeatResponse {
  // Workers associate themselves with a scheduler, and only accept commands
  // from that scheduler. The ID also helps them detect scheduler restarts.
  1: BistroInstanceID id,
  // It's useful for the worker to know when it will be considered lost so
  // that it can commit suicide in the event of a network partition, see
  // README.worker_protocol.  It would also be possible to include these
  // parameters in the healthcheck node ID, but this is more convenient.
  2: i32 maxHealthcheckGap,
  3: i32 heartbeatGracePeriod,
  4: i32 loseUnhealthyWorkerAfter,
  // The worker kills itself this many seconds before the scheduler would
  // consider it lost (this is a safety margin).
  5: i32 workerCheckInterval,
  // Tells the worker when the scheduler moved it from NEW to HEALTHY.
  6: i32 workerState,
}

enum KillMethod {
  SOFT = 1,  // SIGTERM, wait a few seconds, SIGKILL
  HARD = 2,  // SIGKILL
}

// Killing a task can be done with different intentions about its status.
enum KilledTaskStatusFilter {
  // Treat the status as if the task just quit of its own accord.
  NONE = 1,
  // Unless the task is "done", coerce its status to "failed". Useful if you
  // don't want the task to start back up.
  FORCE_DONE_OR_FAILED = 2,
  // Unless the task is "done", coerce its status to "incomplete_backoff".
  // Useful if we're just preempting the task to allow others to run first.
  FORCE_DONE_OR_INCOMPLETE_BACKOFF = 3,
  // Unless the task is "done", coerce its status to "incomplete".  Useful
  // if the task should be eligible to re-run immediately.
  FORCE_DONE_OR_INCOMPLETE = 4,
}
