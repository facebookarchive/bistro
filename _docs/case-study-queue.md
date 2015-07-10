---
id: case-study-queue
title: Task queue
layout: docs
permalink: /docs/case-study-queue/
---

### What's a task queue, anyway?

The traditional task queue is a service, which accepts tasks --- pieces of
work to be executed, together with the computational resources they demand. 
It schedules the jobs according to one of a myriad of heuristics, like
[fair-share scheduling](https://en.wikipedia.org/wiki/Fair-share_scheduling)
and executes them on the appropriate hardware.

There are numerous task queue implementations, both open-source and
proprietary.  Some well-known ones include [Open Grid
Scheduler](http://gridscheduler.sourceforge.net/) and
[Slurm](slurm.schedmd.com) --- Wikipedia has a more exhaustive [list](
https://en.wikipedia.org/wiki/Comparison_of_cluster_software).

### What does Bistro offer?

Bistro's core strength lies in scheduling *data-parallel jobs* (consisting
of many related tasks) with constraints on both *computational resources*
(which are usually interchangeable) and on *data resources* (which are
either unique, or have limited replication).

A traditional task queue is a simple reduction of the above problem:
 
 * Each job consists of just 1 task.
 * Computational resources are treated normally.
 * There are no data resources.

At Facebook, a few deployments have used this very setup, with good success,
and thousands of concurrent jobs.  In Bistro jargon:

 * All jobs use 1 node to produce their 1 task.
 * The node has no data resources.
 * Any worker setup (remote or local) is appropriate.

When does this make sense? Any of the below options might be good reasons.

 * Your deployment needs a flexible resource model. In the aforementioned
   deployments Bistro resources were used to model GPUs, region locality,
   entitlements, etc.
 * Only some of your jobs are data-parallel. This already works in a kludgy
   way, but really clean support is coming soon.
 * Other aspects of Bistro are a good fit -- its APIs or UIs, worker pool,
   execution model, task log & status handling.

One caveat is that Bistro's current public release only supports loading job
configurations [from a
file](https://github.com/facebook/bistro/blob/master/bistro/config/FileConfigLoader.h). 
Our high-performance MySQL-based `ConfigLoader` has too many unreleased
dependencies to make it into the initial release.  However, we would be glad
to guide you in implementing a high-performance, read-write `ConfigLoader`
--- describe your needs in [an
issue](https://github.com/facebook/bistro/issues/new), and we'll help you
out.
