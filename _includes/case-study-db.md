While Bistro can be a capable [traditional task
queue](/bistro/docs/case-study-queue/), one of its more unique features
is the ability to account not just for the resources of the machines
executing the tasks, but also for any bottlenecks imposed by the
*data being queried*.

Here is a simple example:

 * Your data resides on 500 database hosts serving live traffic.
 * Each database host has 10-20 logical databases.
 * The hosts have enough performance headroom to run at most *two*
   batch tasks per host, without impacting production traffic.
 * You have some batch jobs, which must process all the databases.
 * Bistro will use 1 scheduler, and a worker pool of 100 machines.

### Levels

To configure Bistro in such a setup, first describe the structure of the
computation in terms of *nodes* at several levels:

 * Database `host` nodes --- to limit tasks per DB host.
 * Logical `database` nodes, each connected to its DB host node.
   A running task locks its logical DB.
 * If remote worker hosts are in use, a node per `worker` host is
   automatically created.
 * If global concurrency constraints are desired, a top-level scheduler
   `instance` node is also available.

In a typical setup, each running task associates with one node on each level:

 * `database`: The logical DB is the work shard being processed.
 * `host`: What host to query for data?
 * `worker`: Where is the process running?

### Resources

Before running jobs, define resources for the nodes at various levels:

 * `host` nodes should be protected from excessive queries to
   avoid slowing down live production traffic --- allocate 2
   `host_concurrency` slots to honor the requirement above.
 * typical `worker` resources might be, e.g. `cpu_cores`,
   `ram_giagabytes`, etc.  Bistro supports non-uniform resource
   availability among its worker hosts.

### Nodes

Hosts fail, and therefore our `host` to `database` mapping
will change continuously.  To deal with this, Bistro continuously polls for
node updates --- it can periodically run your "node fetcher" script, or read
a file, or poll a custom in-process plugin.  One of the standard plugins
creates and removes nodes corresponding to time intervals, akin to Cron. 
When nodes change, tasks are started on the new nodes, and stopped on the
deleted ones.  Bistro's node model also naturally accommodates database
replicas.

### Jobs

Lastly, specify the jobs to run --- a path to a binary will suffice, and the
binary will be run against every `database` node found above.  Many
other job options and metadata are available.  You can even implement your
tasks as custom RPC requests.

### Ready to go

The above configuration is continuously polled from a file (or other
configuration source) by the Bistro scheduler.  Bistro's worker processes
need only minimal configuration --- tell them how to connect to the
scheduler, and you are in business (they do have plenty of advanced
reliability-related settings).  

You can then interact with the scheduler (and tasks) via HTTP REST, via a
command-line tool, or via a web UI.

Read our <a href="/bistro/static/bistro_ATC_final.pdf">USENIX ATC'15
paper</a> for a more principled explanation of this resource model.  Note
that the model Bistro implements in practice is an extension of the
"resource forest model" that is the focus of the paper.  Bistro's data model
is being refined and improved, so in the future you can expect it to handle
even more applications with ease.

