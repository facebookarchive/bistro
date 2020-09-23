# Bistro: A fast, flexible toolkit for scheduling and running distributed tasks

[![Build Status](https://travis-ci.org/facebook/bistro.svg?branch=master)](https://travis-ci.org/facebook/bistro)

This README is a very abbreviated introduction to Bistro. Visit
http://facebook.github.io/bistro for a more structured introduction, and for the docs.

Bistro is a toolkit for making distributed computation systems. It can
schedule and run distributed tasks, including data-parallel jobs.  It
enforces resource constraints for worker hosts and data-access bottlenecks.
It supports remote worker pools, low-latency batch scheduling, dynamic
shards, and a variety of other possibilities.  It has command-line and web
UIs.

Some of the diverse problems that Bistro solved at Facebook:
 - Safely run map-only ETL tasks against live production databases (MySQL,
   HBase, Postgres).
 - Provide a resource-aware job queue for batch CPU/GPU compute jobs.
 - Replace Hadoop for a periodic online data compression task on HBase,
   improving time-to-completion and reliability by over 10x.

You can run Bistro "out of the box" to suit a variety of different
applications, but even so, it is a tool for engineers.  You should be able
to get started just by reading the documentation, but when in doubt, look at
the code --- it was written to be read.

Some applications of Bistro may involve writing small plugins to make it fit
your needs.  The code is built to be extensible.  Ask for tips, and we'll do
our best to [help](https://www.facebook.com/groups/bistro.scheduler).  In
return, we hope that you will send a pull request to allow us to share your
work with the community.

## Early release

Although Bistro has been in production at Facebook for over 3 years, the
present public release is partial, including just the server components.

## Install the dependencies and build

Bistro needs a 64-bit Linux, Folly, FBThrift, Proxygen, boost, and
libsqlite3.  You need 2-3GB of RAM to build, as well as GCC 4.9 or above.

`build/README.md` documents the usage of Docker-based scripts that build
Bistro on Ubuntu 14.04, 16.04, and Debian 8.6.  You should be able to follow
very similar steps on most modern Linux distributions.

If you run into dependency problems, look at `bistro/cmake/setup.cmake` for
a full list of Bistro's external dependencies (direct and indirect).  We
gratefully accept patches that improve Bistro's builds, or add support for
various flavors of Linux and Mac OS.

The binaries will be in `bistro/cmake/{Debug,Release}`.  Available build
targets are explained here:
   http://cmake.org/Wiki/CMake_Useful_Variables#Compilers_and_Tools
You can start Bistro's unit tests by running `ctest` in those directories.

## Your first Bistro run

This is just one simple demo, but Bistro is a very flexible tool. Refer to
http://facebook.github.io/bistro/ for more in-depth information.

We are going to start a single Bistro scheduler talking to one 'remote'
worker.

Aside: The scheduler tracks jobs, and data shards on which to execute them.
It also makes sure only to start new tasks when the required resources are
available.  The remote worker is a module for executing centrally scheduled
work on many machines.  The UI can aggregate many schedulers at once, so
using remote workers is optional --- a share-nothing, many-scheduler system
is sometimes preferable.

Let's make a task to execute:

```
cat <<EOF > ~/demo_bistro_task.sh
#!/bin/bash
echo "I got these arguments: \$@"
echo "stderr is also logged" 1>&2
echo "done" > "\$2"  # Report the task status to Bistro via a named pipe
EOF
chmod u+x ~/demo_bistro_task.sh
```

Open two terminals, one for the scheduler, and one for the worker.

```
# In both terminals
cd bistro/bistro
# Start the scheduler in one terminal
./cmake/Debug/server/bistro_scheduler \
  --server_port=6789 --http_server_port=6790 \
  --config_file=scripts/test_configs/simple --clean_statuses \
  --CAUTION_startup_wait_for_workers=1 --instance_node_name=scheduler
# Start the worker in another
mkdir /tmp/bistro_worker
./cmake/Debug/worker/bistro_worker --server_port=27182 --scheduler_host=:: \
  --scheduler_port=6789 --worker_command="$HOME/demo_bistro_task.sh" \
  --data_dir=/tmp/bistro_worker
```

You should be seeing some lively log activity on both terminals. In several
seconds, the worker-scheduler negotiation should complete, and you should
see messages like "Task ...  quit with status" and "Got status".

Since we passed `--clean_statuses`, the scheduler will not persist any task
completions that happened during this run.  The worker, on the other hand,
will keep a record of the task logs in `/tmp/bistro_worker/task_logs.sql3`.

If you want task completions to persist across runs, tell Bistro where to
put the SQLite database, via `--data_dir=/tmp/bistro_scheduler` and
`--status_table=task_statuses`

```
mkdir /tmp/bistro_scheduler
./cmake/Debug/server/bistro_scheduler \
  --server_port=6789 --http_server_port=6790 \
  --config_file=scripts/test_configs/simple \
  --data_dir=/tmp/bistro_scheduler --status_table=task_statuses \
  --CAUTION_startup_wait_for_workers=1 --instance_node_name=scheduler
```

You can query the running scheduler via its REST API:

```
curl -d '{"a":{"handler":"jobs"},"b":{"handler":"running_tasks"}}' :::6790
curl -d '{"my subquery":{"handler":"task_logs","log_type":"stdout"}}' :::6790
```

**Pro-tip:** For ease of reading, pipe the output through either `jq` or
`json_pp` (from a Perl package). For longer outputs, try `| jq -C . | less -R`.

You should also take a look at the scheduler configuration to see how its
jobs, nodes, and resources were specified.

```
less scripts/test_configs/simple
```

For debugging, we typically invoke the binaries like this:

```
gdb cmake/Debug/worker/bistro_worker -ex "r ..." 2>&1 | tee WORKER.txt
```

When configuring a real deployment, be sure to carefully review the `--help`
of the scheduler & worker binaries, as well as the documentation on
http://facebook.github.io/bistro.  And don't hesitate to ask for help in the group:
https://www.facebook.com/groups/bistro.scheduler

## License

See [LICENSE](LICENSE).
