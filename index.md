---
layout: default
title: A fast, flexible toolkit for scheduling and running distributed tasks
id: home
hero: true
---

Bistro is a toolkit for making services that schedule and execute tasks.  It
is an engineer's tool --- your clients need to do large amounts of
computation, and your goal is to make a system that handles them easily,
perfomantly, reliably.

You should consider using Bistro instead of rolling your own solution,
because in our experience at Facebook, every heavily used task-execution
service eventually has to build out a very similar set of layers:

* **Execution shell:** Start, wait for, kill tasks
* **Worker pool:** Maintain the distributed state of what tasks are running where
* **Data model:** Currently available tasks; resource accounting
* **Configuration:** Add/remove jobs; tune parameters
* **Persistence:** If the service crashes, don't forget task completions
* **Scheduling logic:** Priorities; dependencies; resource constraints;
host placement
* **Task logs:** Make logs accessible, while controlling disk usage
* **Command-line tools:** Scriptable, yet friendly
* **Web UI:** Minimal learning curve; great for visualizing data
* **RPC API:** When command-line scripting is too slow or clunky

Bistro provides all of the above, and improves continuously, so instead of
re-inventing, you can focus on innovating.

## Early-release software

Although Bistro has been in production at Facebook for over 3 years, the
present public release is partial, including just the server components. 
The CLI tools and web UI will be shipping shortly.

The good news is that the parts that are released are continuously verified
by build & unit tests on <a
href="https://travis-ci.org/facebook/bistro">Travis CI</a>, so they should
work out of the box.

There is already a lot of code, with which to play, so you have any thoughts
--- <a href="https://www.facebook.com/groups/bistro.scheduler/">send us a
note</a>!  Open-source projects thrive and grow on encouragement and
feedback.

## Case study: data-parallel tasks with data <b>and</b> worker resources

While Bistro is a capable traditional task queue, one of its more unique
features is the ability to account not just for the resources of the
machines executing the tasks, but also for any bottlenecks imposed by the
<i>data being queried</i>.

Here is a simple example:

 * Your data resides on 500 database hosts serving live traffic.
 * Each database host has 10-20 logical databases.
 * The hosts have enough performance headroom to run at most <i>two</i> 
   batch tasks per host, without impacting production traffic.
 * You have some batch jobs, which must process all the databases.
 * Bistro will use 1 scheduler, and a worker pool of 100 machines.

#### Levels

To configure Bistro in such a setup, first describe the structure of the
computation in terms of <i>nodes</i> at several levels:

 * Database <tt>host</tt> nodes --- to limit tasks per DB host.
 * Logical <tt>database</tt> nodes, each connected to its DB host node.
   A running task locks its logical DB.
 * If remote worker hosts are in use, a node per <tt>worker</tt> host is
   automatically created.
 * If global concurrency constraints are desired, a top-level scheduler
   <tt>instance</tt> node is also available.

In a typical setup, each running task associates with one node on each level:

 * <tt>database</tt>: The logical DB is the work shard being processed.
 * <tt>host</tt>: What host to query for data?
 * <tt>worker</tt>: Where is the process running?

#### Resources

Before running jobs, define resources for the nodes at various levels:

 * <tt>host</tt> nodes should be protected from excessive queries to
   avoid slowing down live production traffic --- allocate 2
   <tt>host_concurrency</tt> slots to honor the requirement above.
 * typical <tt>worker</tt> resources might be, e.g. <tt>cpu_cores</tt>,
   <tt>ram_giagabytes</tt>, etc.  Bistro supports non-uniform resource
   availability among its worker hosts.

#### Nodes

Hosts fail, and therefore our <tt>host</tt> to <tt>database</tt> mapping
will change continuously.  To deal with this, Bistro continuously polls for
node updates --- it can periodically run your "node fetcher" script, or read
a file, or poll a custom in-process plugin.  One of the standard plugins
creates and removes nodes corresponding to time intervals, akin to Cron. 
When nodes change, tasks are started on the new nodes, and stopped on the
deleted ones.  Bistro's node model also naturally accommodates database
replicas.

#### Jobs

Lastly, specify the jobs to run --- a path to a binary will suffice, and the
binary will be run against every <tt>database</tt> node found above.  Many
other job options and metadata are available.  You can even implement your
tasks as custom RPC requests.

#### Ready to go

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

## Flexible along many dimensions

A Bistro deployment can be tuned in a variety of ways:
 
 * Persistence: no database is required, but any can be integrated
 * Code execution: co-located with the scheduler, or on remote workers
 * Configuration: live-update files, or a more custom solution
 * Data shards: manually configured, periodic, or script-generated

## Check back soon!

We are literally putting together this as you read, so check back in a week
or two for:

 * A "Getting Started" guide
 * Detailed feature documentation
 * Ways to deploy, integrate, and extend Bistro
 * How to contribute to the project

{::comment}
XXX TODOs:
 - Add a #gethelp anchor, for README.md to link to.
   => fb group, issue tracker
 - replace <a <i <b and other < with markdown syntax
 - add instructions on how to start code a la what's in README.md
 - extend the "Flexible" section?

You can configure it to do useful work out-of-the-box, without writing new
code.  As more plugins are written, its utility as a toolkit will increase.

= Bistro as a library =

Another way to think about Bistro is --- it is a library, which you can
use to build various kinds of distributed computing services
{:/comment}
