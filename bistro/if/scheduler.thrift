/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

include "common/fb303/if/fb303.thrift"
include "bistro/bistro/if/common.thrift"
include "bistro/bistro/if/bits.thrift"

namespace cpp facebook.bistro
namespace py facebook.bistro.scheduler

cpp_include "<unordered_map>"
cpp_include "<unordered_set>"

typedef map<string, i32> (cpp.template = "std::unordered_map") stringToIntMap
typedef set<string> (cpp.template = "std::unordered_set") stringSet

struct BistroJobConfigFilters {
  1: stringSet whitelist,
  2: string whitelistRegex,
  3: stringSet blacklist,
  4: string blacklistRegex,
  5: double fractionOfNodes = 1.0,
  6: stringSet tagWhitelist,
}

typedef map<string, BistroJobConfigFilters>
  (cpp.template = "std::unordered_map")
    stringToFiltersMap

struct BistroJobConfig {
  1: string name,
  2: bool enabled = 0,
  3: string owner,
  // Map of resource name => resources required
  4: stringToIntMap resources,
  5: string config,
  6: double priority = 1.0,
  7: stringToFiltersMap filters,
  8: string error,
  9: list<i32> backoffValues,
  10: bool backoffRepeat,
  // createTime & modifyTime may not be set -- only valid if they're positive.
  11: i64 createTime = 0,
  12: i64 modifyTime = 0,
  13: string levelForTasks,
  14: string levelForHostPlacement,
  15: list<string> dependsOn,
  16: string hostPlacement,
  17: optional double killOrphanTasksAfterSec,
  19: common.TaskSubprocessOptions taskSubprocessOptions,
  20: common.KillRequest killRequest,
  // ConfigLoaders can use this to implement compare-and-swap for saveJob(),
  // preventing two concurrent calls from silently clobbering one another.
  //
  // Default to -1 because we need the "no version ID specified" behavior to
  // be equivalent to "add new job" (rather than "update existing job") and
  // -1 is the sentinel for this.
  18: i64 versionID = -1,
}

struct BistroCountWithSamples {
  1: i32 count,
  2: list<string> samples,
}

struct BistroJobHistogram {
  1: string job,
  2: map<string, map<bits.BistroTaskStatusBits, BistroCountWithSamples>>
    statuses,
}

exception BistroSchedulerUnknownJobException {
  1: string message,
} (message = 'message')

service BistroScheduler extends fb303.FacebookService {

  ////
  //// The following calls comprise the scheduler's external API.
  ////

  list<BistroJobConfig> getJobs();
  BistroJobConfig getJob(1: string job_name);

  // Add a new job to bistro.
  void saveJob(1: BistroJobConfig job);

  void deleteJob(1: string job_name)
    throws (1: BistroSchedulerUnknownJobException ex);

  list<string> getLevels();

  list<BistroJobHistogram> getJobHistograms(
    1: list<string> jobs,
    2: i32 samples,
  );

  /**
   * These two sister calls return sequential log lines ordered by line ID,
   * for the given job ID and node ID.  Line ID is an opaque primary key for
   * log lines.  Its first ordering component is guaranteed to be the
   * timestamp.
   *
   * You can request lines starting from a timestamp, or from a line ID.
   * With ascending sort order, the time/line ID you specify will be the
   * minimal one to show up in your results -- otherwise, it'll be the
   * maximal.  Setting line_id to LogLine::kNotALineID means "do not
   * filter on line ID".
   *
   * The number of results you get is implementation-dependent, but you are
   * guaranteed to get *some* lines as long as lines are available in your
   * chosen direction.  The absence of results signals that you can stop
   * paging.
   *
   * To get the next page of results, call getJobLogsByID with
   * LogLines.nextLineID from your previous call.  If no more lines exist,
   * constains kNotALineID.  To get the previous page, keep the line ID (or
   * time) the same, and toggle is_ascending.
   *
   * N.B. The backend supports querying multiple jobs & nodes at once (think
   * of it as WHERE {job, node} in <list>.  If you need it, adding this more
   * powerful call is easy.
   */

  common.LogLines getJobLogsByTime(
    1: string logtype,  // "stdout" or "stderr" or "statuses"
    2: string job_id,
    3: string node_id,
    4: i32 time,  // min if is_ascending, max otherwise
    5: bool is_ascending,  // Order in which to look through the logs
    6: string regex_filter = "",  // Match all lines by default
  );

  common.LogLines getJobLogsByID(
    1: string logtype,  // "stdout" or "stderr" or "statuses"
    2: string job_id,
    3: string node_id,
    4: i64 line_id,  // min if is_ascending, max otherwise
    5: bool is_ascending,  // Order in which to look through the logs
    6: string regex_filter,
  );

  /**
   * Forgive failures of the given job, such that all its unfinished tasks
   * are eligible to run immediately.  If the current status "uses backoff",
   * the backoff duration is reset to 0.  Also, "permanent failure" becomes
   * "error, but can run again".
   */
  void forgiveJob(1: string job_name);

  ////
  //// The following calls are part of the internal scheduler-worker API.
  //// The overview documentation is in worker.thrift.
  ////

  /**
   * Used by workers to report back when a task has stopped running.
   *
   * Usually, each runTask corresponds to a single successful
   * updateStatus(task ended) (or multiple identical retries).  If this
   * invocation's runTask experienced a partial failure, then it may get
   * both an updateStatus(task ended) and an updateStatus(was not running),
   * in either order.  The former overwrites, but is never overwritten by,
   * the latter.  So, for these cases, "was not running" is just a transient
   * state that has no effect on the invocation's final status.
   *
   * Robustness:
   *  - [failure] The worker will retry indefinitely until it succeeds.
   *  - [partial failure] The scheduler may be free to immediately start the
   *    next invocation of the task, which requires caution in two ways:
   *      (i)  If it starts on different worker, the scheduler quietly
   *           ignores the inevitable retry by the previous worker, since
   *           the retry will have the wrong invocation ID.
   *      (ii) If the scheduler tries to start the next invocation on this
   *           same worker, the worker will throw instead of starting it,
   *           since it believes that the old task is still running.
   *  - [identity] The scheduler checks the task ID, invocation ID,
   *    scheduler ID, and worker ID, which means that either the right
   *    status will be updated, or the call will fail.  On mismatched ID,
   *    this always throws, except for the case of mismatched invocation ID
   *    -- we quietly ignore those messages since that almost indicates an
   *    update that is no longer needed.
   *  - [replay] This happens if updateStatus has a partial failure on the
   *    worker. On the scheduler, TaskStatusSnapshot::updateStatus logs, and
   *    refuses to update a "not running" status (except for "was not
   *    running", which we do want to overwrite, as explained above).
   *  - [delay] A delay without replay cannot be problematic, since the
   *    task's next invocation cannot start without the scheduler receiving
   *    the previous updateStatus.
   *  - [ordering/interaction] See README.worker_protocol.
   */
  void updateStatus(
    1: common.RunningTask running_task,
    2: string status,
    // This is not as useful as it seems, since the worker sends whatever
    // happens to be its currently associated scheduler ID.  The worker
    // cannot record the ID that launched the task, because if it did, we
    // would lose the ability to restart schedulers.  Still, this can detect
    // some extremely pathological cases, so leaving it in.
    //
    // TODO(#5023846): If a worker becomes unavailable due to a network
    // partition, then the scheduler restarts, then a new worker comes up
    // with the same shard ID, the original worker will be associated with a
    // scheduler that does not exist.  That means we'll call updateStatus
    // forever.  See a similar note in RemoteWorker.cpp for a fix.
    3: common.BistroInstanceID scheduler,
    4: common.BistroInstanceID worker,
  );

  /**
   * This is how the scheduler discovers new workers. The time the last
   * heartbeat was received is also used to compute health state.
   *
   * Robustness:
   *  - [failure] The worker will retry until it fails its self-health-check,
   *    and will then commit suicide.
   *  - [partial failure] If this is a new worker, the scheduler will
   *    attempt getRunningTasks, which will fail due to a scheduler ID
   *    mismatch.  Otherwise, there will simply be a mismatch in the
   *    worker's and scheduler's health states.  All issues resolve with the
   *    next successful heartbeat.
   *  - [identity] We deliberately don't address this to any particular
   *    scheduler, since this is used for worker discovery, and workers thus
   *    end up cold-calling schedulers.  Instead, the discovered scheduler
   *    identifies itself in the return value, and the worker always uses
   *    the latest ID (to track the right instance as the scheduler hostport
   *    source changes over time).
   *  - [replay & delay] Replayed or delayed messages can cause the worker's
   *    and scheduler's health states to diverge temporarily, but they do
   *    not cause to much damage, since, e.g.  healtchecks are redundant and
   *    also have to be functioning for tasks to be dispatched to a worker.
   *    If useful, heartbeats & responses could each have a skew-adjusted
   *    TTL, or sequence numbers.
   *  - [ordering/interaction] See README.worker_protocol.
   *
   * NB It could be neat to count any successful interaction with the worker
   * as a heartbeat (and therefore send heartbeats less often), but at the
   * moment, there is no clear need to muddy up the logic this way, since
   * heartbeats are cheap.
   */
  common.SchedulerHeartbeatResponse processHeartbeat(
    1: common.BistroWorker w,
    // When a scheduler restarts, it inspects the IDs being sent by
    // connecting workers to determine if all previous workers had already
    // reconnected.  If we achieve complete consensus -- all connected
    // workers report the same ID, which is also the ID of the set of
    // connected workers -- then the scheduler exits its "initial wait".
    2: common.WorkerSetID workerSetID,
  );

}
