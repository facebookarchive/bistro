---
docid: configuration
title: Configuration
layout: docs
permalink: /docs/configuration/
---

# Storing Configuration

Bistro's configuration is formatted as JSON, and comes in two components:

- `bistro_settings`: Configuration for the whole scheduler.
- `bistro_job->NAME`: Settings for a named job. 

Bistro allows many implementations of its `ConfigLoader` interface. Any loader will get polled periodically to refresh to settings and list of jobs. The new configuration will be used on the next scheduling iteration.

Loaders may implement the `saveJob` (create or update a job) and `deleteJob` methods, which allows Bistro to act as a job queue without the clients being aware of how it stores the configuration. `saveJob` pre-parses configuration, and, in case of errors, stores the config with a special `errors` field.

Bistro ships with:

- [FileConfigLoader](https://github.com/facebookarchive/bistro/blob/main/bistro/config/FileConfigLoader.h): Reads a file containing a JSON dict with keys as above. Does not currently implement job mutations.

# Command-line arguments

Both `bistro_scheduler` and `bistro_worker` also support a variety of command-line flags via [GFlags](https://gflags.github.io/gflags/). Unlike regular configuration, these can only be changed by restarting the process. To begin learning about flags, try `--help`, and `grep` the codebase for `FLAGS_name_of_flag` as necessary. 

*Caveat*: Read the help descriptions carefully — due to the design of GFlags, the `bistro_worker` binary inherits some flags from the remote-protocol library that it does not actually use.

# Bistro Settings

The only key required to be present in the config is `bistro_settings`. This determines the overall settings for the scheduler. Supported keys include:

- `enabled`: If set to `false` then Bistro will not run any jobs. Default: **true**.
- `working_wait`: The time in seconds for Bistro to wait between scheduler iterations if it has running tasks. Default: **0.5** seconds.
- `idle_wait`: The time in seconds for Bistro to wait between scheduler iterations if it does not have running tasks. Default: **5** seconds.
- `scheduler`: The scheduler policy to use. Default: **"roundrobin"**. The full list is [here](https://github.com/facebookarchive/bistro/blob/main/bistro/scheduler/SchedulerPolicies.cpp) (check [Config.h](https://github.com/facebookarchive/bistro/blob/main/bistro/config/Config.h#L169) for the strings).
- `backoff`: The default backoff settings to use for each job. The format is a list of integer values that represent the backoff time in seconds. For example: [5, 15, 60] would mean that a task backs off for 5 seconds after one failure, 15 seconds after two, and 60 seconds after 3 failures. After 4 or more failures the task fails permanently. Optionally, the last entry of the array can be the string "repeat", which will repeat the last backoff value for all additional failures (thus there can never be a permanent failure). *Note:* backoff times must be distinct integers in increasing order. Default: **[15, 30, 60, 300, 900, 3600, 7200, 21600, 86400, "repeat"]**. Search [Overview of Concepts](https://facebook.github.io/bistro/docs/overview-of-concepts/) for a note on `forgive_jobs`, which allows you to get out of backoff or permanent failure.
- `nodes` *(required)*: See "Node and Resource Settings" below.
- `resources` *(required)*: See "Node and Resource Settings" below.
- `node_order`: For each job, the scheduler sequentially checks each node to see if a task can be run on it. This setting determines the order of these checks. Defaults to **"random"**. Other options are [here](https://github.com/facebookarchive/bistro/blob/main/bistro/config/NodeOrderType.h).
- `level_for_tasks`: See "Running Jobs on Different Levels" below.
- `physical_resources`: See [Physical resources](https://facebook.github.io/bistro/docs/physical-resources/).
- `remote_worker_selector`, `worker_resources_override`, `CAUTION_exit_initial_wait_before_timestamp`: These settings control resource control and execution for Bistro's remote worker pool. To learn more, you will want to start with [Overview of concepts: RemoteWorkerRunner](https://facebook.github.io/bistro/docs/overview-of-concepts/#remoteworkerrunner), and then proceed to read the code.
- `kill_orphan_tasks_after_sec`, `task_subprocess`, `kill_subprocess`: See [Supervising and killing tasks](snarkmaster.github.io/bistro/docs/supervising-and-killing-tasks) for the details.

## Determining which tasks run (or do not run), when

Sometimes, you want one task to run before another. If this is a preference, but **not a hard constraint**, then you should look into job `"priority"` values and the Bistro-level `"scheduler"` policy setting. You can also change the Bistro-level "node_order".

If certain tasks must **definitely not run**, then you should look at job-level configs of `"filter"`, `"depends_on"`, and `"run_only_after_all_nodes_are_done_for"`.

If you want tasks to run at certain times / on a schedule, check out the ["add_time" node source](https://github.com/facebookarchive/bistro/blob/main/bistro/nodes/AddTimeFetcher.h#L25).

## Node and Resource Settings

Read [Nodes and Resources](facebook.github.io/bistro/docs/nodes-and-resources/) to learn these concepts in depth.

### Nodes

The `nodes` key in `bistro_settings` configures the node sources that Bistro will use. This key is required (otherwise no computation could occur). The value for `nodes` should be an object with two required keys: `levels` and `node_sources`. The `levels` key is an array that names *in order* the levels the node sources can use, e.g. `["top", "middle", "bottom"]`. The number of levels depends on the node source settings. The `node_sources` key determines exactly which node fetchers to use, along with associated options.

### Resources

The `resources` in `bistro_settings` key restricts the tasks that can run concurrently against a specific unit of data, globally, or on a worker host.

This key is required, although it's possible to set it to an empty object to have no resource constraints (be careful!). This object describes resources available at each node level. It has the form `{"level name": {"resource_name": {...}}`. A resource object has these keys:
 * `default` *(required)*: excepting jobs that customize how much of this resource they require, every task of every job will need consume this much of the resource whenever it uses a node of this level.
 * `limit` *(required)*: the number of slots of that resource that is available at each node on that level (`bistro_settings → worker_resources_override` can change this for workers).
 * `weight` *(optional)*: How important is this resource when `remote_worker_selector` is set to `busiest`? The mechanism is documented in [Overview of concepts: RemoteWorkerRunner](https://facebook.github.io/bistro/docs/overview-of-concepts/#remoteworkerrunner).

## Example `bistro_settings`

``` json
"bistro_settings": {
  "working_wait" : 0.25,
  "idle_wait" : 10,
  "scheduler" : "roundrobin",
  "enabled" : true,
  "nodes" : {
    "levels" : [
      "db_host",
      "db"
    ],
    "node_sources" : [
      {
        "source" : "manual",
        "prefs" : {
          "aardvark.example.com": {},
          "bobcat.example.com": {"disabled": true},
          "capybara.example.com": {},
        }
      }, 
      { 
        "source": "range_label",
        "prefs": {
          "parent_level": "db_host",
          "format": "{parent}:db_{i}",
          "start": 1,
          "end": 5,
        }
      }
    ]
  },
  "resources" : { 
    "instance" : {
      "global_concurrency" : {
        "limit" : 500,
        "default" : 0
      },
    },
    "db_host" : {
      "host_concurrency" : {
        "limit" : 2, 
        "default" : 1
      }
    },
    "db" : {
      "db_concurrency" : {
        "limit" : 1, 
        "default" : 1
      }
    }
  }
}
```

This specifies a 3-level node tree, containing 19 nodes: 

- 1 `instance` node, with 500 slots of `global_concurrency` available → 
    - 3 `db_host` nodes (`[abc]*.example.com`), each with 2 slots of `host_concurrency` available →
        - 15 `db` nodes (`[abc]*.example.com:db_[1-5]), each with 1 slot of `db_concurrency` available.

# Job Settings

In addition to the `bistro_settings` key, each Bistro config can support 0 or more keys for configuring jobs. For `FileConfigLoader` and similar storage mechanisms, each job key should begin with `bistro_job->` followed by the job name (stick to alphanumerics with underscores or dashes under 240 characters). Each job is configured as a JSON object. Supported keys include:

- `enabled`: A boolean that determines whether we will run the job. Default: **true**. See also `kill_orphan_tasks_after_sec`.
- `owner` *(required)*: A string representing the owner of the job. 
- `command`: An array of `["command", "initial", "args"]`. The job will run this command instead of the `--worker_command` passed to `bistro_scheduler` (with `LocalRunner`) or `bistro_worker` (with `RemoteWorkerRunner`).  Also see [Task execution](https://facebook.github.io/bistro/docs/task-execution).
- `priority`: A floating-point priority value, higher values are usually more important. Its effect depends on the scheduler policy used. Defaults to **1.0**.
- `config`: This is an arbitrary JSON object that will be passed to each task. This is where custom configuration per-job should go. Defaults to **{}**.
- `resources`: *(overrides `bistro_settings`)* A JSON object that determines how much of each resource a job uses (overriding the defaults set in the `bistro_settings`). The format is `{"RESOURCE_NAME": amount_used}`.
- `backoff`: *(overrides `bistro_settings`)* A JSON list that overrides the global `backoff` setting for a specific job. See `bistro_settings` for the format.
- `filters`: See "Job Filters" below.
- `depends_on`: If the configuration for "job3" sets `"depends_on": ["job1", "job2"]`, then "job3" will only start on node "n" after both "job1" and "job2" have finished on "n". Defaults to **[]**.
- `run_only_after_all_nodes_are_done_for`: Syntactically, this is like `"depends_on"`, but you must use it with **extreme care**. If in your use-case, the set of nodes changes while jobs run, this is **not** for you. If you want a fuzzy definition of "the job is done", like "99.9% of all nodes are done, and 100% of the 1000 most critical nodes are done", then this option is **not** for you. If you use this option, your job will **only** run if all of the following conditions are met:
    - It is eligible to run under all the other criteria ("depends_on", "enabled", etc).
    - All of the jobs in the list are "done", which means that each dependency:
        - Is eligible to run now -- disabled jobs are not "done", even if all nodes are done.
        - Has a known `level_for_tasks`. If we can't look up the nodes that must be done, the job is not "done".
        - All of its nodes either have the status "done", or are filtered out. If your level exists, but has 0 nodes, your job is done.
        - It is "done" in the current configuration -- if Bistro was reconfigured, "done"-ness is always evaluated relative to the latest configuration.
- `level_for_tasks`: *(overrides `bistro_settings`)* See "Running Jobs on Different Levels" below.
- `kill_orphan_tasks_after_sec`, `task_subprocess`, `kill_subprocess`: *(overrides `bistro_settings`)* See [Supervising and killing tasks](snarkmaster.github.io/bistro/docs/supervising-and-killing-tasks) for the details.
- `host_placement`, `level_for_host_placement`:  *CAUTION* — these options are likely to change significantly in the future. Allows scheduling a job on specific hosts, primarily useful for scheduling tasks on the hosts that contain the data. Note that both options use the actual hostname of the worker process (from the structure [MachinePortLock](https://github.com/facebookarchive/bistro/blob/main/bistro/if/common.thrift)), not the worker's ID. If both `host_placement` and `level_for_host_placement` are set, the former prevails since it is more specific.
- `errors`: If you the `ConfigLoader` contains an invalid job, or if the  `save_job` REST call received an invalid job, Bistro will do a best-effort parse of the JSON, and record any bad data in the `errors` key — see [DynamicParser.h](https://github.com/facebook/folly/blob/master/folly/experimental/DynamicParser.h) for a description of its format.

## Running Jobs on Different Levels

By default Bistro runs tasks on the last node level (the leaves of the tree).

By setting `level_for_tasks` in `bistro_settings` or on a specific job, you can run jobs on  different levels. For example, you could run a maintenance job at the host level, and a data-processing job at the database level.

It can be tempting to create multiple Bistro deployments for different jobs. This is OK only if these deployments do not share any resources, e.g., each job only consumes resources at its own level. If they do share resources, there should be one Bistro deployment for resource control.

## Job Filters

Each job supports a `filters` key, which allows it to specify exactly which nodes to run on. The default is to apply no filters, hence a job by default will run on all nodes. A jobs `filter` has the format `{"LEVEL_NAME": {"FILTER_TYPE": ...}}`. The job will start a task on a node only if the node satisfies **all the filters** — i.e. we are taking a set intersection. The filters for each level support the following options. 

- `whitelist`: *Only* run on nodes in this list.
- `whitelist_regex`: Run only on nodes matching this `boost::regex`-compatible regular expression.
- `blacklist`: A list of nodes to skip. 
- `blacklist_regex`: Nodes in matching the regex will be skipped.
- `fraction_of_nodes`: A value from 0 to 1 that deterministically selects a fraction of nodes to run. This is useful for testing on a small fraction before running the job on all nodes. The hash function used to pick the subset of nodes is not guaranteed to be consistent across different builds of `bistro_scheduler`.
- `tag_whitelist`: Each node can have a set of tags. This is a list of tags on which the job wants to run. Nodes that don't have any of the tags listed will be skipped.

## Example Job Settings

This job is **heavier** than normal, since it uses 2 out of 2 of the `host_concurrency` resource, rather than the default of 1.

``` json
"bistro_job->single_db_test_job": {
  "enabled" : true,  
  "owner" : "msklodowskacurie",
  "filters" : {
    "db" : {
      "whitelist" : ["aardvark.example.com:db_7"]  
    }
  },
  "config" : {
    "some_custom_data_for_the_job" : 1000
  },
  "priority" : 1,
  "resources": {"host_concurrency": 2}
}
```
