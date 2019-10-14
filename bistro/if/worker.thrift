/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

include "common/fb303/if/fb303.thrift"
include "bistro/bistro/if/common.thrift"

namespace cpp facebook.bistro
namespace py facebook.bistro.worker

/***************************************************************************
 *** Read README.worker_protocol, and keep it up-to-date with this file. ***
 ***************************************************************************/

exception BistroWorkerException {
  1: string message,
} (message = 'message')

service BistroWorker extends fb303.FacebookService {

  /**
   * Invoked whenever a worker connects to a scheduler for the first time.
   * This means the scheduler can be restarted without losing running tasks.
   *
   * Rationale: One alternative is to include the full set of running tasks
   * with every heartbeat.  However, these updates can arrive out of order
   * relative to runTask and updateStatus, making it very hard to properly
   * reconcile the modifications to the scheduler state, even with global
   * sequence numbers.  Reconciliation becomes easier with per-task sequence
   * numbers, but this has a high implementation complexity, and a
   * significant performance overhead with tens of thousands of tasks per
   * second.  Instead, we only send the full list of running tasks when a
   * worker first connects, and rely on robust implementations of runTask &
   * updateStatus to keep the scheduler's state in sync with its workers.
   *
   * Robustness:
   *  - [failure]: If the call fails, the scheduler will keep the worker in
   *    a NEW (not healthy) state, retrying frequently, until it either
   *    succeeds, or the worker reaches its suicide timeout.
   *  - [partial failure, replay, delay]: Read-only query, needs no protection.
   *  - [identity] Checks worker ID. Does not check the scheduler ID, since
   *    this often arrives just before the worker associates with a new
   *    scheduler. This is a read-only call, so the only consequence of
   *    responding to the wrong scheduler is that this scheduler may e.g.
   *    move this worker from a NEW to a HEALTHY state, and try to runTask.
   *    Of course, its runTask()s would fail, so no real harm is done.
   */
  list<common.RunningTask> getRunningTasks(
    1: common.BistroInstanceID worker
  );

  /**
   * Start the requested task on the worker if the worker considers itself
   * HEALTHY, or if the task is a healthcheck.
   *
   * If this succeeds, it is guaranteed that the worker will follow up with
   * updateStatus (or die, in which case its tasks usually die too).  If
   * this throws a BistroWorkerException, it is guaranteed that the task is
   * not running, and can be retried safely.  For any other error, the
   * scheduler may neither assume that the task is running nor that it is
   * not running.  Instead, it must use notifyIfTasksNotRunning to find out.
   *
   * Robustness:
   *  - [failure] The only bona fide failure is when the worker throws
   *    BistroWorkerException, and that gets delivered successfully to the
   *    scheduler.  In that case, the scheduler knows the task is not
   *    running, and it will retry, likely on a different worker.
   *  - [partial failure] On any other kind of failure (e.g. a transport
   *    error), the scheduler is neither sure that the task is running nor
   *    that it is not running.  It will check via notifyIfTasksNotRunning.
   *  - [replay] A replay would occur if the scheduler sends a second
   *    runTask while a task with the same task ID (job + node, not task
   *    instance ID) is already running.  This could only happen if the
   *    worker considers a task running, but the scheduler does not.  A
   *    "normal" reason for this to happen is that updateStatus(task ended)
   *    has a partial failure.  Throws if a task with the same ID is already
   *    running (the code also CHECKs that the a invocation ID is never sent
   *    twice, but the CHECK should be changed to an exception if this is
   *    actually possible).
   *  - [delay] If a delay is long enough that the scheduler decided via
   *    notifyIfTasksNotRunning + updateStatus(was not running) that the
   *    task is not running, then the runTask will fail the sequence number
   *    check, and throw.  Otherwise, the scheduler still considers the task
   *    to be running, and there is no harm in starting the task a bit late.
   *  - [identity] Throws if the scheduler ID or the worker ID does not
   *    match (except if the task is a special "new worker" healthcheck, in
   *    which case the scheduler ID is not checked, see code).
   */
  void runTask(
    1: common.RunningTask rt,  // replay protection: check task, invocation IDs
    // The config passed to the task, the 3rd JSON command-line argument.
    2: string config,
    // Binary to run. If empty, run the default worker command.
    3: list<string> command,  // node, status, and config are the 3 arguments
    // Identify the originating scheduler, and the the worker, for which
    // this task was intended.  In the event of mistaken identity, the
    // worker must refuse to start the task.  One instance of mistaken
    // identity arises from this race:
    //
    //  1) Old worker dies, new worker starts at the same address.
    //  2) Scheduler sends a task to the new worker.
    //  3) The new worker reports a heartbeat, making the scheduler assume
    //     that all running tasks are dead, including the one just sent.
    //  4) The worker receives the task and starts running it.
    //
    // This is a huge problem when workers don't report running tasks, since
    // the scheduler would lose track of the task for as long as it runs.
    // For task-reporting workers, the race would result in the scheduler
    // transiently believing that the task is not running for one
    // heartbeatPeriod.  The risk of a duplicate task being scheduled in
    // that time is also quite high.
    4: common.BistroInstanceID scheduler,
    5: common.BistroInstanceID worker,
    // This is a per-worker counter tracked by both the scheduler & the
    // worker to ensure that runTask does not start tasks, which the
    // scheduler believes not to be running due to notifyIfTasksNotRunning.
    // It starts at 0 when worker associates with the scheduler, and is
    // incremented just before a notifyIfTasksNotRunning is sent to that
    // worker.  runTask is always sent with the counter's current value.
    //
    // The worker has to reject a runTask because the scheduler may already
    // have received an updateStatus(was not running) for that task in
    // response to a notifyIfTasksNotRunning.  Let's say the runTask arrives
    // with a counter of C1.  Then it must be rejected if the worker's
    // counter is > C1, e.g.:
    //
    //   send runTask / counter = 1
    //   send notifyIfTasksNotRunning / counter = 2
    //     can only be querying tasks with counter < 2
    //   send runTask / counter = 2
    //   receive notifyIfTasksNotRunning / counter = 2
    //     bumps worker's counter, replies "not running" to task w/ counter = 1
    //   receive runTask / counter = 1
    //     rejected, since the worker's counter 2 is greater than the task's 1
    //   receive runTask / counter = 2
    //     started
    //
    // In other words, a worker receiving runTask will decline to run tasks
    // whose counter is less than its own counter.  This means that a
    // notifyIfTasksNotRunning call may cause the failure of some
    // immediately preceding runTask calls.  Since the latter call is a
    // response to a runTask transport failure, this should be rare enough
    // not to affect throughput.
    6: i64 notify_if_tasks_not_running_sequence_num,
    7: common.TaskSubprocessOptions opts,
  ) throws (1: BistroWorkerException ex);

  /**
   * Used by the scheduler to learn whether a task is actually running when
   * runTask failed on the scheduler, but may have succeeded on the worker.
   *
   * If this call succeeds, then the scheduler assumes that all the
   * enumerated tasks are running, until notified otherwise.  The worker
   * will then notify the scheduler via updateStatus(was not running) of the
   * tasks that were not actually running, retrying indefinitely.
   *
   * Robustness:
   *  - [failure] The scheduler retries indefinitely, with bounded
   *    exponential backoff.
   *  - [partial failure, replay, delay] Read-only query, needs no protection.
   *  - [identity] Throws if the the worker ID or scheduler ID does not
   *    match, forcing the scheduler to retry the call (while assuming that
   *    the task is still running).  Logs on tasks whose invocation IDs do
   *    not match, but does not reply to the scheduler via updateStatus --
   *    the reason being that such mismatches can happen if
   *    notifyIfTasksNotRunning is delayed.
   *
   * == Rationale ==
   *
   * There are a few reasons I chose to poll the read-only
   * notifyIfTasksNotRunning() instead of retrying runTask() periodically
   * until the worker is lost.
   *
   * Practically speaking, read-only polling is simpler than pushing with
   * side effects:
   *  - Retries are a superset of the logic that was added for
   *    notifyIfTasksNotRunning.  Specifically, they would incur extra
   *    complexity in RemoteWorkerRunner, since the retry logic would have
   *    to do curry all the state needed to run the task.  They would also
   *    need to assert that the worker and scheduler states are still
   *    compatible with running the task.
   *  - Retries are much more expensive, both because they are not batched,
   *    and because they involve bigger requests.
   *  - Retries would deliberately try to runTask() on known-unhealthy
   *    workers.  Bugs in this state would have worse consequences than if a
   *    known-unhealthy worker is polled via notifyIfTasksNotRunning(), so
   *    this increases the test burden.
   *  - The retry code has to be careful regarding sequencing with future
   *    invocations of the same task.
   *
   * Philosophically, read-only polling follows the "do no further harm"
   * creed, which is appropriate given that Bistro tries never to exceed
   * resource constraints or start duplicate tasks:
   *  - With notifyIfTasksNotRunning, each iteration leads to increasing
   *    certainty that the task is NOT running, so that by the time the
   *    worker is lost, we're pretty darn sure.
   *  - With retries, every new attempt _might_ start the task the task
   *    without the scheduler knowing about it (just like the first
   *    attempt), which means that under some conditions (e.g. the worker
   *    -> scheduler network routes are congested), we could even increase
   *    the probability that there's a task running unknown to the
   *    scheduler.  This seems like a bad idea in the face of the unreliable
   *    communications that caused the retry in the fist place.
   *
   * Last, but not least, there are many ways to implement "poll to check if
   * the task is running".  Here are two rejected alternatives:
   *
   * - Instead of a sequence number, use a TTL with runTask, and wait longer
   *   than the TTL to send the first notifyIfTasksNotRunning().  Requires
   *   the worker to estimate the clock-skew, and adjust the TTL
   *   accordingly.  Not implemented due to excessive complexity.
   *
   * - Instead of notifyIfTasksNotRunning(), use a synchronous
   *   areTasksRunning() call, whose return value indicates which tasks are
   *   **NOT** running.  It is a mistake to indicate the tasks that are
   *   truly running, because it's best that the worker treats tasks with
   *   wrong invocation IDs the same way as running tasks.  It is much
   *   harder to reason about inter-call interactions in this setup, since
   *   this adds yet another way to mutate task status on the scheduler.
   */
  void notifyIfTasksNotRunning(
    1: list<common.RunningTask> rts,  // checks task & invocation IDs
    3: common.BistroInstanceID scheduler,
    4: common.BistroInstanceID worker,
    5: i64 notify_if_tasks_not_running_sequence_num,  // See runTask comment
  );

  /**
   * If this succeeds, the worker commits suicide before the request is
   * over.  So, the scheduler's side of the request _always_ fails.
    *
   * Robustness:
   *  - [failure] If the transmission or the worker side fails, and the
   *    worker stays alive, the scheduler will repeat the requestSuicide
   *    whenever it receives another heartbeat from the worker.
   *  - [partial failure] Does not apply -- the scheduler side always fails.
   *  - [replay, delay] The worker does not implement replay or delay
   *    protection.  For associated workers, this is okay, because the
   *    scheduler changes its state before dispatching a suicide request in
   *    such a way as to reject any future updates from this worker.  If the
   *    call fails, the scheduler will reply with "suicide" to every
   *    subsequent heartbeat it receives.
   *
   *    For non-associated workers, this bad sequence of events is possible:
   *     - scheduler sent requestSuicide (since the current worker was ok)
   *     - worker sent & got response to a 2nd heartbeat, and got associated
   *     - worker received the (much delayed) requestSuicide
   *     - worker died
   *    TODO(#5024460): (lo-pri) Fix by e.g. adding a timestamp to the
   *    request and refusing requests that are too old.  Doing this right
   *    would require require reviving the clock-skew measurement diff.
   *  - [identity] The IDs in the request prevent suicide of the wrong
   *    worker, and requests from the wrong scheduler.
   *  - [ordering] There are no ordering issues with other calls since the
   *    worker dies the moment it processes this request.
   *
   * TODO(#5497731): It may be better not to check the scheduler ID, since
   * that way a new / restarted scheduler can kill an old worker (which may
   * be running tasks) that was temporarily inaccessible due to a network
   * partition.
   */
  void requestSuicide(
    1: common.BistroInstanceID scheduler,
    2: common.BistroInstanceID worker,
  ) throws (1: BistroWorkerException ex);

  /**
   * Kill the given running task invocation. Throws if the running task has
   * the same ID, but a different invocation ID.  Does nothing if no task
   * with this ID is running.  Throws on various other errors.
   *
   * Robustness:
   *  - [failure] Currently, task termination is initiated by clients of
   *    the scheduler.  If it fails, it's up to the client to retry.
   *  - [partial failure] The scheduler need not defend against "send failed
   *    / receive succeeded", since tasks may die at any time anyway.
   *  - [replay] Safe, since this task invocation will no longer be running.
   *  - [delay] No need to check, because the scheduler really meant to kill
   *    that specific task invocation, and there is no way for it to cancel
   *    that intention.
   *  - [identity] Cannot accidentally kill the wrong task or invocation,
   *    since the task & invocation ID is verified (as are the worker &
   *    scheduler IDs).
   *  - [ordering] There are no ordering issues with any other calls, since
   *    neither the worker nor the scheduler change their "running task"
   *    state after killTask() -- instead, they wait for the task's exit, or
   *    for its updateStatus, respectively. NB: kill can arrive before start.
   */
  void killTask(
    1: common.RunningTask rt,
    // 2: DEPRECATED
    // Verifying these IDs is pretty redundant given the running task
    // invocation ID, but better safe than sorry, and it might anyhow be a
    // sensible thing to do if Bistro gets speculative execution.
    3: common.BistroInstanceID scheduler,
    4: common.BistroInstanceID worker,
    5: common.KillRequest req,
  ) throws (1: BistroWorkerException ex);

  /**
   * Returns sequential log lines, whose jobID and nodeID match the given
   * lists, starting with line_id (in either ascending or descending order).
   *
   * The number of results you get is implementation-dependent, but you are
   * guaranteed to get *some* lines as long as lines are available in your
   * chosen direction.  The absence of results signals that you can stop
   * paging.
   *
   * To get the next page of results, use LogLines.nextLineID in your next
   * call (it's set to kNotALineID if there are no more lines).  To get the
   * previous page, keep line_id the same, and toggle is_ascending.
   *
   * Robustness:
   *  - [failure] Currently, log line queries are initiated by clients of
   *    the scheduler.  If one fails, it's up to the client to retry.
   *  - [partial failure, replay, delay] Read-only query, needs no protection.
   *  - [identity] Logs are supposed to persist across worker instances, so
   *    don't check the worker ID.  At present, it's not harmful to return
   *    logs to a non-associated scheduler, so don't check the scheduler ID.
   *  - [ordering] Does not meaningfully interact with "running tasks"
   *    state, so there are no ordering issues with the other worker calls.
   */
  common.LogLines getJobLogsByID(
    1: string logtype,  // "stdout" or "stderr" or "statuses"
    2: list<string> job_ids,
    3: list<string> node_ids,
    4: i64 line_id,  // Get >= lines if is_ascending, =< otherwise
    5: bool is_ascending,  // Order in which to look through the logs
    6: i32 limit,
    7: string regex_filter,
  ) throws (1: BistroWorkerException ex);

}
