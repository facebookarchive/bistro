---
docid: supervising-and-killing-tasks
title: Supervising and killing tasks
layout: docs
permalink: /docs/supervising-and-killing-tasks/
---

# Automatically killing tasks that should not be running

By default, Bistro does not kill tasks belonging to jobs or nodes that were
deleted or disabled.  This can be changed via `kill_orphan_tasks_after_sec`. 
It can be set to:

 * not set: Defaults to "do not kill" unless you've set a deprecated gflag for this
 * `false` or a negative number: Do not kill
 * `true`: Kill immediately, synonym for `0`.
 * number: Kill after this many seconds (to cope with transiently deleted jobs or nodes). Can be fractional.

Also refer to 
[Physical Resources](https://bistro.io/docs/physical-resources/)
for notes on RAM & CPU enforcement.

**Caveat:** If the resources available on nodes decrease or disappear,
already-running tasks may end up requirin resources they do not have. At
present, Bistro will not kill such tasks, but fixing this would not be hard.

# Managing and terminating task processes

It is impossible to reliably kill subprocess trees using POSIX system calls. Bistro supports a few variations of killing from:

* just signaling the child -- either immediately, or `TERM`-wait-`KILL`.
* making the child a process group leader, and signaling the process group
* passing a "canary" pipe FD to the child to have a snitch that tells it whether any of its descendants are alive
* using cgroups to reliably contain all processes in the task

More detailed discussion of non-cgroup killing strategies can be found in
[README.task_termination](https://github.com/facebookarchive/bistro/blob/main/bistro/processes/README.task_termination).

All the non-cgroup strategies can be configured using just `bistro_settings`
(see `parseTaskSubprocessOptions`, `parseKillRequest`).  Using the `cgroups`
setting also requires a properly configured cgroup slice, such that Bistro
has write access to the cgroup directory subtrees that it needs.

You will want to review the copiously-documented structures in
[common.thrift](https://github.com/facebookarchive/bistro/blob/main/bistro/if/common.thrift):
`TaskSubprocessOptions`, `CGroupOptions`, `KillRequest`.

It is not too hard to set up the system so that `cgroups` works — make a slice on `systemd` distributions, or manually make a directory on others -- and set permissions appropriately.

Note that at present, Bistro does not support `cgroup-v2`, but adding
this feature would be quite straightforward.

Here is an example configurations to help you with the syntax:

```
"bistro_settings": {
    ...,
   
    "task_subprocess": {
        "use_canary_pipe": true,
        "parent_death_signal": 9,
        "cgroups": {
            "kill_without_freezer": false,
            "subsystems": [
                "cpu",
                "memory",
                "cpuacct",
                "freezer"
            ],
            "slice": "bistro.slice",
            "root": "/sys/fs/cgroup"
        },
        "max_log_lines_per_poll_interval": 50,
        "process_group_leader": true,
        "poll_ms": 100
    },
    "kill_request": {
        "method": "term_wait_kill",
        "kill_wait_ms": 15000
    }
}
```
