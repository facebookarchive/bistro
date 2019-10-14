/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <folly/experimental/ThreadedRepeatingFunctionRunner.h>
#include <folly/MPMCQueue.h>
#include <folly/Synchronized.h>
#include <memory>
#include <string>

#include "bistro/bistro/if/gen-cpp2/BistroWorker.h"
#include "bistro/bistro/if/gen-cpp2/common_types.h"
#include "bistro/bistro/if/gen-cpp2/scheduler_types.h"
#include "bistro/bistro/processes/TaskSubprocessQueue.h"
#include "bistro/bistro/remote/RemoteWorkerState.h"
#include "bistro/bistro/statuses/TaskStatus.h"
#include "common/fb303/cpp/FacebookBase2.h"

namespace apache { namespace thrift { class ThriftServer; }}

namespace facebook { namespace bistro {

namespace cpp2 {
  class BistroSchedulerAsyncClient;
}
class UsablePhysicalResourceMonitor;

class BistroWorkerHandler final : public cpp2::BistroWorkerSvIf,
                                  public fb303::FacebookBase2 {
public:
  // Must be thread-safe.
  typedef std::function<std::shared_ptr<cpp2::BistroSchedulerAsyncClient>(
    folly::EventBase* event_base
  )> SchedulerClientFn;
  // IMPORTANT: This logger MUST be thread-safe, noexcept, and fast.
  typedef std::function<
    void (const char*, const cpp2::BistroWorker&, const cpp2::RunningTask*)
  > LogStateTransitionFn;

  BistroWorkerHandler(
    std::weak_ptr<apache::thrift::ThriftServer>,
    const boost::filesystem::path& data_dir,
    LogStateTransitionFn,  // Must be thread-safe, noexcept, and fast
    SchedulerClientFn,
    const std::string& worker_command,
    // How can external clients connect to this worker?
    const cpp2::ServiceAddress& addr,
    // What local port is this worker locking? (see MachinePortLock)
    int32_t locked_port
  );
  ~BistroWorkerHandler() override;

  // Never return UNHEALTHY because we take care of our own suicide.
  fb303::cpp2::fb_status getStatus() override {
    return fb303::cpp2::fb_status::ALIVE;
  }

  void getRunningTasks(
    std::vector<cpp2::RunningTask>& out_running_tasks,
    const cpp2::BistroInstanceID& worker
  ) override;

  void runTask(
    const cpp2::RunningTask& rt,
    const std::string& config,
    const std::vector<std::string>& command,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    int64_t notify_if_tasks_not_running_sequence_num,
    const cpp2::TaskSubprocessOptions& opts
  ) override;

  void notifyIfTasksNotRunning(
    const std::vector<cpp2::RunningTask>& rts,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    int64_t notify_if_tasks_not_running_sequence_num
  ) override;

  // Instead of blocking until the request succeeds, schedules the
  // time-consuming work on the current thread's EventBase thread.
  void requestSuicide(
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker
  ) override;

  void killTask(
    const cpp2::RunningTask& rt,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    const cpp2::KillRequest&
  ) override;

  void getJobLogsByID(
    cpp2::LogLines& out,
    const std::string& logtype,
    const std::vector<std::string>& jobs,
    const std::vector<std::string>& nodes,
    int64_t line_id,
    bool is_ascending,
    int limit,
    const std::string& regex_filter
  ) override;

  /**
   * The implementation of 'requestSuicide': worker stops accepting new
   * tasks, kills existing tasks, and tells its Thrift server to stop
   * serving.
   *
   * Rationale: It is exposed for the signal handler. Doing this seems
   * better than the janky requestSuicide(getSchedulerID() getWorker().id),
   * which would then generate "server requested suicide" logging.
   */
  void killTasksAndStop() noexcept;

  /**
   * The functions below are used in unit tests only
   */
  RemoteWorkerState getState() const {
    return state_.copy();
  }
  cpp2::BistroWorker getWorker() const {
    return worker_;
  }
  cpp2::BistroInstanceID getSchedulerID() const {
    return schedulerState_->id;
  }

private:
  // TODO: Replace this with a common Thrift struct throughout Bistro &
  // worker, use it in RunningTask, etc.  (job, node are required)
  typedef std::pair<std::string, std::string> TaskID;

  LogStateTransitionFn logStateTransitionFn_;
  SchedulerClientFn schedulerClientFn_;
  const std::string workerCommand_;

  struct NotifyData {
    const TaskID taskID;
    const TaskStatus status;

    NotifyData(
      TaskID task_id,
      TaskStatus status
    ) : taskID(std::move(task_id)), status(std::move(status)) {}
  };

  void throwIfSuicidal();  // Used to disable thrift calls when shutting down.

  // Don't run Thrift calls for another worker or from the wrong scheduler.
  void throwOnInstanceIDMismatch(
    const std::string& func_name,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker
  ) const;

  // Helper for heartbeat & healthcheck -- state_ must be locked.
  void setState(RemoteWorkerState*, RemoteWorkerState::State, time_t);

  // Background threads
  std::chrono::seconds notifyFinished() noexcept;
  std::chrono::seconds notifyNotRunning() noexcept;
  std::chrono::seconds heartbeat() noexcept;
  std::chrono::seconds healthcheck() noexcept;

  TaskSubprocessQueue taskQueue_;
  folly::MPMCQueue<std::unique_ptr<NotifyData>> notifyFinishedQueue_;
  folly::MPMCQueue<cpp2::RunningTask> notifyNotRunningQueue_;

  // Heartbeats report tasks as running from the time they are queued via
  // runTask until the scheduler has successfully been notified of their
  // completion.  Watch out: this includes healthchecks, even though the
  // scheduler does not track them.
  //
  // Can be locked together with state_; if so, lock this second.
  //
  folly::Synchronized<std::unordered_map<TaskID, cpp2::RunningTask>>
    runningTasks_;

  const boost::filesystem::path jobsDir_;

  // This gets sent out in every heartbeat. throwOnInstanceIDMismatch() uses
  // the ID to ensure that only Thrift calls intended for this worker are
  // processed. The heartbeat period controls the heartbeat() thread.
  const cpp2::BistroWorker worker_;  // Const => no need to synchronize

  // This health state is maintained by the healthcheck() thread. The
  // worker will refuse to start tasks if it's unhealthy, and will commit
  // suicide just before it would be lost by the scheduler.
  //
  // Can be locked together with runningTasks_; if so, lock this first.
  // Can be locked together with schedulerState_; if so, lock this first.
  folly::Synchronized<RemoteWorkerState> state_;

  // This is sent by the scheduler in response to worker heartbeats.  The
  // included scheduler ID determines the Thrift requests that will be
  // allowed by throwOnInstanceIDMismatch().  Also, healthcheck() uses the
  // scheduler's timeouts to determine worker health.
  //
  // CAUTION: It's only okay to lock this for trivial operations (e.g.
  // copies / assigns / reads), not any longer.  If you must lock it
  // together with state_, then you should lock state_ first, since its lock
  // can be held for much longer.
  //
  // DO: It may be somewhat cleaner to use a single lock for both state_ and
  // schedulerState_ (since most of the values are timeouts that are only
  // used by healthcheck()), and keep a separate synchronized copy of the
  // scheduler ID for use in the other functions.
  folly::Synchronized<cpp2::SchedulerHeartbeatResponse> schedulerState_;

  // When a new scheduler instance connects, send it our running tasks.
  bool gotNewSchedulerInstance_;  // Used only by the heartbeat() thread.

  // Before we sent out any heartbeats, the worker makes sure that our
  // "external" IP address actually works, at least locally.  This is also
  // prevents us from sending the heartbeat before the worker's Thrift
  // server is up.
  bool canConnectToMyself_;  // Used only by the heartbeat() thread.

  // Flipped from true to false when the scheduler starts to commit suicide.
  // While the worker kill its tasks, this has the effect of blocking new
  // tasks from running.
  std::atomic_bool committingSuicide_{false};

  // This monitor is created and updated from `runTask()`, but is accessed
  // from the heartbeat thread, so it should be synchronized.
  struct UsablePhysicalResources {
    // null until the first healtcheck is received.
    std::unique_ptr<UsablePhysicalResourceMonitor> monitor_;
    // These options describe the current monitor. They should change
    // rarely.  When they do, we update the monitor.
    cpp2::CGroupOptions cgroupOpts_;
  };
  folly::Synchronized<UsablePhysicalResources> usablePhysicalResources_;

  // To suicide gracefully, the handler needs to be able to stop its server.
  // Must be a weak_ptr to avoid a circular server<->handler dependency.
  std::weak_ptr<apache::thrift::ThriftServer> server_;

  // CAUTION: Declared last since the threads access other members of `this`.
  folly::ThreadedRepeatingFunctionRunner backgroundThreads_;
};

}}
