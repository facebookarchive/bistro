---
docid: task-execution
title: Task execution, arguments and environment
layout: docs
permalink: /docs/task-execution/
---

# Command-line arguments

Bistro invokes your task binary with three arguments

```
command node_name status_pipe config_JSON
```

- `command` is the path to your binary, set as part of the 
  [job settings](https://bistro.io/docs/configuration/), or
  passed via `--worker_command` to the scheduler for `LocalRunner`,
  otherwise to the worker.

- `node_name` is described in [Nodes and Resources](https://bistro.io/docs/nodes-and-resources/).
  It tells for your binary which data shard to process.

- `status_pipe` is a temporary file for communicating the task status to
  Bistro.  You should write only one line to it after the task is done or
  throws an error.  This line a can be a plain
  [Task Status](https://bistro.io/docs/overview-of-concepts/)
  string like `done`, `incomplete`, or `error_backoff`. It can also be
  a JSON object of the form 

  ```
  {"result": "done", "data": {"your data up to a few KB"}}
  ```

  Instead of `result`, you could also use `result_bits`, see [bits.thrift](https://github.com/facebookarchive/bistro/blob/main/bistro/if/bits.thrift).

  For example, Bistro always marks the task done if `command` is the
  following script:

  ```
  echo "done" > "$2"
  ```

- `config_JSON` includes extra arguments for your binary.  By default it has:
  - `id`: your job name
  - `path_to_node`: an array of node names from root to `node_name`
  - `prev_status`: status result in JSON of the last run of this task
  - `config`: all key value pairs in "config" of your job configuration. 
    The "config" field allows you to change command line arguments for
    future task invocations while your job runs, without restarting Bistro.

# Logging

Bistro sets up your task with stdout and stderr file descriptors, which it
reads line-by-line (lines have a optional maximum length), timestamps,
rate-limits (see 
["max_log_lines_per_poll_interval" in "Managing task processes"](https://bistro.io/docs/supervising-and-killing-tasks/)),
and writes to a SQLite database on the local disk.

To retrieve the logs, send the scheduler a `task_logs`
[REST request](https://bistro.io.io/docs/rest-api/), 
read 
[handleTaskLogs() for the details](https://github.com/facebookarchive/bistro/blob/main/bistro/server/HTTPMonitor.cpp).

# Working directory

A given Bistro worker (or a `LocalRunner` scheduler) starts all tasks of the
same job in the sam directory: `--data_dir` / `jobs` / `<NAME OF JOB>`.
