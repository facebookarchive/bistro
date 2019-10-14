/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp facebook.bistro
namespace php bistro
namespace py facebook.bistro.common

// Marks LogLines that are not authentic log lines, but which instead store
// some kind of error or warning about the log line fetching process.
const i64 kNotALineID = -1;
// The current protocol version, by default we reject connections from others.
// == Changelog ==
//  v1: Add WorkerSetID to SchedulerHeartbeatResponse, to be echoed
//      by the worker with the next processHeartbeat.
//  v2: Require a worker update since the new scheduler cannot know the
//      correct duration to wait for tasks to get killed when an
//      older-protocol worker is lost.  If needed, it is ok to revert the
//      version bump since this is a "corner case" fix with a sane fallback.
const i16 kProtocolVersion = 2;


// NB: As implemented, GPUInfo in both the 'usable' and 'task' context is
// always more stale than the msSinceEpoch in the containing
// *PhysicalResources structs.  If you need timestamps on the GPUInfo, query
// them on the nvidia-smi command-lines, and add them to this struct.
struct GPUInfo {
  1: string name  // E.g. model name
  2: string pciBusID
  3: double memoryMB
  4: double compute  // 1 is available, and 15% utilization == 0.15
}

struct UsablePhysicalResources {
  1: i64 msSinceEpoch = 0
  2: double cpuCores = 0  // Fractional cores __could__ be supported
  3: double memoryMB = 0
  4: list<GPUInfo> gpus
}

struct TaskPhysicalResources {
  1: i64 msSinceEpoch
  // The scheduler can figure CPU cores by dividing CPU time by wall time.
  // CAVEAT: The kernel tracks nanoseconds, so if your 40-core box stays hot
  // for 14 years, this will wrap around.  You have been warned.
  2: optional i64 cpuTimeMs
  3: optional double memoryMB
  4: list<GPUInfo> gpus
}

// Both members are required because we keep hundreds of millions of these
// in memory, and therefore don't want to waste bits on the __isset field.
struct BackoffDuration {
  // The job is out of retries, it failed permanently.
  1: required bool noMoreBackoffs,
  // Wait this long before trying again.
  2: required i32 seconds,
}

struct LogLine {
  1: string jobID,
  2: string nodeID,
  3: i32 time,
  4: string line,
  5: i64 lineID,  // Use lineID to start paging from this log line.
}

struct LogLines {
  // Can contain "error" lines whose lineID == kNotALineID
  1: list<LogLine> lines,
  // Contains the lineID of the next page, kNotALineID if no more lines
  2: i64 nextLineID,
}

struct Resource {
  1: required string name,
  2: required i32 amount,
}

struct NodeResources {
  1: required string node,
  2: required list<Resource> resources,
}

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
  // This field is returned by the worker to the scheduler on
  // getRunningTasks, letting the scheduler poll for task resource usage.
  //
  // The __isset bit here lets us distinguish whether resources were never
  // queried (not set), or if they aren't available (set to default).
  7: optional TaskPhysicalResources physicalResources,
  // The scheduler dictates this to the workers, because this value is part
  // of the scheduler's "safe wait after a task is lost" calculation.
  //
  // It's kind of lame for this to be per-task, but it makes the code easier.
  8: i32 workerSuicideTaskKillWaitMs
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

// Used for identifying a set of workers via their startTime and rand.  This
// is not the most efficient use of bits, but it has the advantage of being
// obviously commutative and invertible.  The odds of collision are very low
// even just on account of xor(worker1.id.rand, ..., workerN.id.rand), but
// if you want better odds, add Galois field multiplication.
struct SetHash {
  1: i64 addAll = 0,
  2: i64 xorAll = 0,  // aka Galois field addition
}

// Lets a newly restarted scheduler decide whether the currently connected
// workers are exactly the same as those it had before the restart.
struct WorkerSetHash {
  1: SetHash startTime,
  2: SetHash rand,
  3: i32 numWorkers = 0,
}

struct WorkerSetID {
  // Each scheduler instance maintains an incremental history of worker set
  // versions, which is much more efficient than storing a set of [shard
  // name, worker instance id] pairs per worker.
  1: BistroInstanceID schedulerID,
  // Logically, the version increases every time the set of connected
  // workers changes, but it should be able to overflow safely.  Therefore
  // you must NEVER compare versions directly, and instead use the
  // overflow-aware comparator WorkerSetIDEarlierThan.
  2: i64 version,
  // Each scheduler instance has a different version history, so on startup,
  // the scheduler uses just the hash to decide if the set of connected
  // workers equals the previous scheduler's consensus set as reported by
  // the workers.
  3: WorkerSetHash hash,
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
  6: i16 protocolVersion = 0,  // Default must stay at 0
  // Installed system resources, or Bistro's cgroup share thereof.
  7: UsablePhysicalResources usableResources,
}

enum PhysicalResource {
  // Enforcement: NONE and HARD. SOFT can be added, either via cgroups
  // OOM-notifier, or (more flaky) by monitoring & killing tasks.
  RAM_MBYTES = 1,
  // Enforcement: NONE and SOFT. A form of HARD could use cpuset.
  // Fractional cores can be supported.
  CPU_CORES = 2,
  GPU_CARDS = 4,
}

// Not all enforcement options are available for all physical resources.
enum PhysicalResourceEnforcement {
  NONE = 1
  // If there is no resource contention, like NONE, otherwise like HARD.
  SOFT = 2
  // Process is killed upon exceeding resource limit, or is denied the
  // ability to exceed tit.
  HARD = 3
}

// This is in Thrift only because it provides pretty-printing and comparators.
struct PhysicalResourceConfig {
  1: PhysicalResource physical
  // Any user-specified worker resource name, and its numeric ID.
  2: string logical
  3: i64 logicalResourceID
  4: double multiplyLogicalBy = 1  // 1024 takes logical GB to physical MB
  // What to do if tasks exceed their allocation in this resource?
  5: PhysicalResourceEnforcement enforcement = NONE
  // When mapping physical resources onto worker logical resources, mark
  // some portion of resources as off-limits -- a simple way of reserving
  // e.g. CPU or RAM for Bistro and/or OS needs.
  6: double physicalReserveAmount = 0
}

struct CGroupOptions {
  // A cgroup for a particular task consists of four components:
  //  root/subsystem/slice/task/
  // Some example values:
  //   root: /sys/fs/cgroup
  //   subsystem: cpuset
  //   slice: schedulers/bistro
  //   task: 20151127235604:fbc3e1d89aa2764f:234098
  // The task subgroup name is generated in TaskSubprocessQueue.
  1: string root
  2: string slice
  // If this is nonempty, then for each new task, we will make a task cgroup
  // inside our slice inside each subsystem, and will place the task's
  // subprocess into the task cgroups.  All per-task cgroups are marked
  // `notify_on_release`, but no other garbage-collection is provided -- you
  // should set up your the root `release_agent` for each subsystem to
  // `rmdir` emptied cgroups.  Bistro need not run as root; instead just
  // ensure that it can create subdirectories in each of the paths in this
  // map -- making dedicated parent cgroups for Bistro, and `chown`ing or
  // ACLing them to be writable by Bistro's user is most appropriate.  As a
  // side effect, you can set resource limits on all Bistro jobs as a whole.
  3: list<string> subsystems
  // In normal cgroups, control files are pre-made by sysfs. It's hard to
  // replicate this in unit tests, so instead we only add O_CREAT during
  // unit tests.
  4: bool unitTestCreateFiles = 0,
  // If nonzero, limit CPU shares for the task. Maps to `cpu.shares` in the
  // `cpu` subsystem.  The sanest way to use this is to set every task's
  // number of shares to to something like 64 * number of requested cores.
  // Then, so long as the scheduler's # cores is correct, each task is
  // guaranteed at least as many cores as it requests.  IMPORTANT: Linux
  // does not allow this value to be 1, so ensure it's >= 2.
  5: i16 cpuShares = 0,
  // If nonzero, sets a hard limit on the amount of memory used.
  6: i64 memoryLimitInBytes = 0,
  // CAUTION: Enabling this will expose you to the possibility of signaling
  // the wrong process due to the race of "we read a cgroup, a process
  // exits, its PID is reused, we send the signal".  If your PIDs are
  // 16-bit, this is definitely a BAD idea.  Don't do it.  With 32-bit PIDs,
  // you will usually be OK, but not immune, especially with long-running
  // tasks.
  //
  // By default, cgroup-based kill is disabled unless the `freezer`
  // subsystem is enabled for the task.  Since there are some concerns about
  // the reliability / stability of `freezer`, this flag provides an "out",
  // enabling cgroup-based kill even if `freezer` is missing.  If `freezer`
  // is enabled, it will be used regardless of this flag.
  7: bool killWithoutFreezer = 0,
  // Future: one could easily allow writing values to specific files in
  // specific subsystems, but this flexibility seems more error-prone and
  // unnecessary as of now.
}

// If you change the defaults, keep in mind that these must be appropriate
// for Bistro's health-check tasks.
struct TaskSubprocessOptions {
  // Every N milliseconds, check whether a task process had exited. Also
  // used for log line rate limits, taskMaxLogLinesPerPollInterval_.  It
  // is a bad idea to make this less than 2ms.
  1: i32 pollMs = 10  // Wake up 100 times per second

  // Every poll interval, increase each task's log line quota by this
  // much.  With the defaults of 50 lines every 10ms, 5000 lines per
  // second can be logged by each task (either to stdout or stderr)
  // without being throttled.  If you want per-job rate limits, just
  // introduce a resource to control task concurrency appropriately.
  // Non-positive values disables the rate limiting entirely.
  2: i32 maxLogLinesPerPollInterval = 50  // 50*(1000/10) = 5k lines/s

  // (Linux-specific) If Bistro exits, what signal should its child
  // processes receive?  Use 0 for 'no signal', 15 for SIGTERM, 9 for
  // SIGKILL.  README.task_termination explains the KILL
  3: i32 parentDeathSignal = 9

  // When true, makes each task's child process a group leader, and signals
  // the entire process group to kill the task.  See README.task_termination
  // for the details.  Using cgroupOptions is much more robust, so also
  // enable that, if available.
  //
  // Defaults to `false` so that Ctrl-Z and Ctrl-C work when running
  // Bistro interactively in a terminal.
  4: bool processGroupLeader = 0

  // This is a hack to allow more reliable waiting for descendants of the
  // task's descendant processes.  Specifically, waitpid(-pgid) does not
  // work in POSIX (though Linux does provide PR_SET_CHILD_SUBREAPER),
  // since as soon as our child exits, its orphaned descendants get
  // reparented to `init` and can no longer be waited for.  To provide
  // *some* way of waiting for orphaned descendants, we open a pipe to the
  // child, and pray that neither it nor its descendants will choose to
  // close random FDs they do not know about.  As long as the canary pipe
  // is open, we know that some descendant is still running, and the task
  // will not be considered complete.  Most child processes won't close
  // the status pipe either, so this hack is entirely optional -- if
  // you're short on FDs, turning it off is harmless.
  5: bool useCanaryPipe = 1

  // Refresh the real resources usage in seconds
  6: i32 refreshResourcesSec = 2

  7: CGroupOptions cgroupOptions
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
  7: i16 protocolVersion = 0,  // Default must stay at 0
  // The worker will send this right back to the scheduler, which enables a
  // freshly restarted scheduler to know when all its old workers are back.
  8: WorkerSetID workerSetID,
}

enum KillMethod {
  TERM_WAIT_KILL = 1,  // SIGTERM, wait, SIGKILL -- formerly 'SOFT'
  KILL = 2,  // SIGKILL -- formerly 'HARD'
  TERM = 3,
}

struct KillRequest {
  1: KillMethod method = KillMethod.TERM,
  // For TERM_WAIT_KILL, how many ms to wait for a child to exit between
  // SIGTERM and SIGKILL.  Non-positive values send SIGKILL immediately.
  2: i32 killWaitMs,
}
