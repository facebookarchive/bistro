---
id: getting-started
title: Getting started
layout: docs
permalink: /docs/getting-started/
---

### Leap into action

The [README](https://github.com/facebook/bistro/blob/master/README.md) gives
the fast path to getting Bistro up and running.  If you cannot wait to get
your hands dirty, go ahead, and start that compile.  I'll wait.

### What just happened? --- a crash course on Bistro

After the installation, the
[README](https://github.com/facebook/bistro/blob/master/README.md) tells you
to:

* Make a `demo_bistro_task.sh`, which Bistro will run for you.
* Start a scheduler:

  ~~~ sh
  bistro_scheduler --server_port=6789 --http_server_port=6790 \
    --config_file=scripts/test_configs/simple --clean_statuses \
    --CAUTION_startup_wait_for_workers=1 --instance_node_name=scheduler
  ~~~
* Start a worker, to be commanded by the scheduler:

  ~~~ sh
  bistro_worker --scheduler_host=:: --scheduler_port=6789 \
    --worker_command="$HOME/demo_bistro_task.sh" --data_dir=/tmp/bistro_worker
  ~~~

Although this is just a toy example, we can learn a lot from it:

* The scheduler uses two fixed ports: 
  * `--server_port` provides a Thrift interface used by the workers.
  * `--http_server_port` provides an HTTP REST API, used for monitoring and
    control.
* The scheduler is configured via a
  [file](https://github.com/facebook/bistro/blob/master/bistro/scripts/test_configs/simple),
  further discussed below.
  * Configuration is constantly refreshed. Bistro polls the file every
    `--config_update_ms` milliseconds.
  * Configuration is
    [pluggable](https://github.com/facebook/bistro/blob/master/bistro/config/FileConfigLoader.h):
    you could use any kind of database instead.
* The other scheduler settings are specific to the demo, but also clarify
  some of Bistro's design:
  * `--CAUTION_startup_wait_for_workers=1` --- by default, Bistro waits a
    fairly long time for remote workers to connect, before starting to run
    tasks.  This prevents double-starting tasks in the event that the
    scheduler restarts during a network partition.  Read the [protocol
    documentation](https://github.com/facebook/bistro/blob/master/bistro/if/README.worker_protocol)
    for the details.  For demo purposes, we abandon safety and shrink the
    wait to just 1 second.
  * Every Bistro scheduler has a root "instance" node, named after the
    hostname of the machine it runs on.  This distinguishes their other
    nodes in the UI in multi-scheduler deployments.  Unfortunately, the
    worker hosts also correspond to nodes named by their hostname, so we add
    `--instance_node_name=scheduler` to rename the scheduler's instance node
    to differ from the hostname.  We could equally well have renamed the
    worker's node via `--shard_id=worker`.
* The worker's configuration is simpler:
  * `--scheduler_host` (IPv6 is evidently ok) and `--scheduler_port` 
    identify the scheduler instance, and the worker registers itself.
  * `--worker_command` is invoked for every new task, although Bistro's
    [task execution is also
    pluggable](https://github.com/facebook/bistro/blob/master/bistro/runners/),
    so if you want your tasks to be custom RPC or even HTTP requests, all it
    takes is a few lines of C++.
  * `--data_dir` is required for two reasons:
    * To keep the logs in `task_logs.sql3`, with automatic truncation,
      see `--help`.
    * To provide a per-job working directory in `jobs/`, useful for scratch
      files, outputs, or inter-task data exchange.
* Lastly, `demo_bistro_task.sh` will be triggered for every `job, level2
  node` pair --- this is the actual work of your sharded computation.  The
  arguments of this process follow a simple protocol:
  * `argv[1]`: The *node* on which to run, a unique string --- a shard ID
    for your computation.
  * `argv[2]`: Write **one** status line into this status file. Valid values
    include `done`, `incomplete`, `error_backoff`, `failed`, but other
    values, including JSON, are also supported.  Bistro ignores your
    process's exit code (these are unreliable in most scripting languages),
    and assumes an error if you did not write exactly one status line.
  * `argv[3]`: JSON of the form:

    ~~~ json
    {
      "config": {...passed through from your job's config...},
      "prev_status": {... the job's previous status on this node ...},
      ... other, more advanced metadata, like nodes & resources ...
    }
    ~~~

    `"config"` lets you change your tasks' configuration on the fly ---
    since job configuration is polled frequently, new tasks are always
    started with the latest `"config"`.  You can use `"prev_status"` as a
    non-durable checkpoint (but [open an
    issue](https://github.com/facebook/bistro/issues/new) if you want to
    improve its durability).

### Understanding the configuration file

Let's go through the demo's [JSON configuration
file](https://github.com/facebook/bistro/blob/master/bistro/scripts/test_configs/simple),
line by line.  We start with the `"nodes"` in the `"bistro_settings"`
section --- this is the part that configures the scheduler.

~~~ json
{
  "bistro_settings" : {
    "nodes" : {
      "levels": [ "level1", "level2"],
      "node_source": "manual",
      "node_source_prefs": {
        "node1": ["node11", "node12"],
        "node2": ["node21", "node22"]
      }
    },
~~~

A `node` is a unique string, which is Bistro's way of making tasks and
tracking resources.  To make a task, you need:

  * a job
  * a logical node, which identifies the unique shard being worked on -- see 
    `argv[1]` above
  * some number of resource nodes, which serve to enforce resource constraints

Each node belongs to one `level`. The scheduler's sole instance node belongs
to the special `instance` level.  The workers' nodes belong to the special
`worker` level.  Any other number of levels can be created, modeling data
resources.  Non-`worker` levels are ordered, starting with `instance`. 
Nodes have parents, with the parent always belonging to the immediately
preceding level.

A job can designate any level as its `level_for_tasks`, which tells Bistro
how to pick the logical nodes (aka the job's shards).  The default is the
bottom level.

Coming back to the JSON above, it describes two levels below `instance`.
There are two nodes at `level1`, both having the instance node as their
parent.  They have two children each, e.g.  `node1` has `level2` descendants
`node11` and `node12`.  Unless a job specifies otherwise, its
`level_for_tasks` will be `level2`.

When Bistro makes tasks for a job, it takes all the nodes from its
`level_for_tasks`.  For each node, we get a task --- it "uses" all the
node's parents, all the way up to the `instance`.  The tasks's nodes
must have enough `"resources"` for it to run:

~~~ json
    "resources": {
      "instance": {"concurrency": {"limit": 3, "default": 1}}
    },
~~~

The `"resources"` entry in `"bistro_settings"` says:

* Every node at the `instance` level (there's just one) has 3 slots of the
  resource named `concurrency`.
* By default, a job uses 1 slot (but jobs can easily override this).

In other words, this Bistro scheduler will not run more than 3 tasks, unless
some jobs use 0 `"concurrency"` slots.

~~~ json
    "enabled" : true
  },
~~~

The scheduler will not start any new tasks unless `"bistro_settings"` sets
`"enabled"` to `true`.

We now come to the job configuration. `FileConfigLoader`'s JSON file format
distinguishes jobs via the `bistro_job->` prefix, so this deployment has
just one job named `simple_job`.  The required parameter `"owner"` is
typically the name or UNIX username of the person responsible for the job.

~~~ json
  "bistro_job->simple_job" : {
    "owner" : "test",
    "enabled" : true
  }
}
~~~

A job will not start any tasks unless it is `"enabled"`.  Moreover, the
`"kill_orphan_tasks_after_sec"` option (in `"bistro_settings"` or per-job)
can be set to a non-negative value to kill already-running tasks if their
job becomes disabled, or their node is deleted (see `nodes_update_ms` in the
code).

### Your choices will differ
   
You can see that the demo made a lot of implementation choices, but they are
not set in stone.  Configuration and plugins let you:

 * Avoid having `bistro_worker`s altogether, spawning processes directly
   from the scheduler, or running tasks via network requests.
 * Choose a different way of storing and live-updating the scheduler & job configuration.
 * Persist task statuses in different ways.
 * Select a variety of job / task / resource data models.
 * Choose, or implement [your own scheduling
   heuristic](https://github.com/facebook/bistro/blob/master/bistro/scheduler/RoundRobinSchedulerPolicy.cpp)
 * Tune the task log storage.

In all, Bistro's flexible architecture can implement a great variety of
services, and we will gladly guide you, should you wish to extend it
further.
