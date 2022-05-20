---
docid: physical-resources
title: Physical resources, discovering and enforcing
layout: docs
permalink: /docs/physical-resources/
---

# Physical resources

These are advanced features, which require some familiarity with Linux
process management to use well.  For this reason, this documentation is
intentionally sparse.  You will want to review the code in
[bistro/physical](https://github.com/facebookarchive/bistro/tree/master/bistro/physical)
when enabling these features.

The mentions of physical resources in
[RemoteWorkerRunner.cpp](https://github.com/facebookarchive/bistro/blob/main/bistro/runners/RemoteWorkerRunner.cpp)
are responsible for integrating the capability to query resources with
actual scheduling and enforcement.

Please talk to us if you need this documentation to be fleshed out.

In principle, it would be desirable to extend this mechanism to support
automatic discovery of other resource types — including data resources.  If
you are looking to do this work, please open an issue to discuss the design.

## Discovering worker host physical resources

To handle worker host pools with heterogeneous hardware, Bistro can
automatically discover — and convert into its own internal logical resource
slots:

 - System CPU cores
 - System RAM
 - nVidia GPUs on the system, including the specific GPU type

Discovering available physical resources on a host is tricky business. You
cannot just read `/proc/cpuinfo` and run with it.  Linux provides `cgroup`
and `cgroup-v2` for restricting the resources available to a process tree --
and it is completely reasonable for `bistro_worker` to be sandboxed in this
way.  At present, Bistro's code for discovering usable physical resources is
aware of neither `cgroup` nor `cgroup-v2`.  However, proper cgroup support
would be better -- patches are welcome, see `bistro/physical`.  For one
workaround, see `physical_reserve_amount` below.

On the plus side, besides auto-detecting system memory & CPU cores, Bistro
knows how to query available nVidia GPUs via `nvidia-smi`.  Patches for
other resource types are also welcome.

Here is an example configuration for automatically setting 3 resources on
all `worker`-level nodes:

``` json
"physical_resources": {
    "ram_mb": {
        "logical_resource": "ram_gb",
        "multiply_logical_by": 1024,
        "physical_reserve_amount": 4096
    },
    "cpu_core": {
        "logical_resource": "cpu_core",
        "enforcement": "none"
    },
    "gpu_card": {
        "logical_resource": "gpu"
    }
}
```

If your `worker` level defines logical resource names of the form `GPU:
[Card Name]` (with the card name as returned by `nvidia-smi`'s query field
`name`), the above incantation will also automatically populate those
resources on your workers.

`physical_reserve_amount` is subtracted from the system's resources before
converting physical resources to units of logical slots.  This serves to
reserve a fixed part of the system's resources for non-Bistro services
running on the system.

# Enforcing worker resource consumption

A task may declare it needs 2 GB of RAM and 1 core, but in actuality could
use 10 cores and 20GB of RAM. In a shared compute cluster, this can lead
to instability and poor performance.

Bistro can use Linux `cgroup` subsystems (when appropriately configured, see
[Supervising and killing tasks](snarkmaster.github.io/bistro/docs/supervising-and-killing-tasks))
to enforce CPU & RAM utilization.

At present, `cgroup-v2` is not supported.

In the previous section's `physical_resources` example, you will note that
`cpu_core` has `enforcement` set to `none`.  That demonstrates the syntax
for configuring task resource enforcement.

Bistro has exactly two options:

- RAM enforcement can be `none` or `hard`. In the latter case, the task's
  `memory` subsystem cgroup will be limited to the amount of physical RAM
  that corresponds to the task's declared logical resource requirements.

- CPU enforcement can be `none` or `soft`. In the latter case, the task's
  `cpu` subsystem cgroup will have `cpu.shares` set in proportion to the the
  task's declared logical resource requirements.  This simply assures that —
  in case of CPU contention — the CPU resources allotted to Bistro's cgroup
  slice are distributed among the Bistro tasks in proportion to their
  logical CPU resource specifications.
