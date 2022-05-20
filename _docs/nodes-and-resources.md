---
docid: nodes-and-resources
title: Nodes and resources
layout: docs
permalink: /docs/nodes-and-resources/
---

## Specify your computation using nodes and resources

Your first step in using Bistro will likely be to figure out the node & resource model for your computation.

## `nodes` describe the structure of your computation

Most Bistro use-cases have a set of shards, on which the computation needs to run. To represent these in Bistro, you will use `nodes`, which are a slight generalization of a typical data-sharded computation. If reading academic papers is your thing, the rationale for this model is explained in the [Bistro USENIX ATC paper](https://facebook.github.io/bistro/static/bistro_ATC_final.pdf).

When processing several shards is bottlenecked on a common resource (e.g. databases residing on the same host), the nodes representing those shards will share a `parent` node. Of course, parent nodes can have further parent nodes to represent resource bottlenecks of a larger scope (e.g. a rack switch or a QPS-limited service).

In discrete math terms, the nodes in a Bistro deployment normally form a [graded poset](https://en.wikipedia.org/wiki/Graded_poset) with a single maximal element. For non-mathematicians, 

- Every node has a parent, with the exception of the instance node (defined below).
- Cycles are prohibited. 
- Each node belongs to a `level`. Levels are ordered, starting with the instance level, and going down to the leaf nodes.
- A node's parent should reside at the level above it (level-skipping links are allowed but discouraged, since they are often hard to understand).
- Each scheduler instance has a single node at the `instance` level. The name of the instance node defaults to the scheduler's hostname, and can be customized by passing `--instance_node_name=something` to `bistro_scheduler`.
- Each node's ultimate ancestor is the instance node.

Each job's computations will happen on all the nodes from a single `level_for_tasks`. This level can be set globally in `bistro_settings`, or can be configured per job. This way, you can use a single Bistro deployment to e.g. schedule jobs on database tables, on entire databases, and on the hosts that contain the databases.

## `resources` limit computation concurrency

Bistro's `resources` are just named concurrency slots that are attached to individual nodes. Each node name† has a table mapping "resource name" to "integer number of slots", e.g.

``` json
{
  "cpu_cores": 6,
  "ram_gb": 30,
  "iops": 200
}
```

† * **Caveat:** Bistro has a very narrow case, aka a **hack**, which permits multiple resource tables to exist for a single node name. The motivation for this feature is to allow running read-only jobs against read-only replicas of MySQL databases. You are explicitly discouraged from trying to make this work on your own, but if having this feature is essential to your success, please talk to us.*

### How many resources does a node get?

Given a node, the available number of slots for its resource is normally determined by the node's level via this key in `bistro_settings`:

``` json
"resources": {"LEVEL_NAME": {"RESOURCE_NAME": {
  "default": NUMBER_OF_SLOTS_JOBS_REQUIRE_BY_DEFAULT,
  "limit": NUMBER_OF_SLOTS_AVAILABLE_ON_NODES_FROM_THIS_LEVEL,
}}
```

For the special case of resources corresponding to Bistro remote workers, one can set `worker_resources_override` in `bistro_settings` to support a heterogeneous pool of computing resources. Or, `physical_resources` provides a way to map the worker's actual physical resources to logical slots in Bistro's resource model. In most senses, Bistro's remote workers (see the binary `bistro_worker`) are modeled as a special node level called `worker`, although they lack parents and have some special logic that ought to eventually be refactored away.

A job running on a node ("task" for short) can require any number of resource slots by specifying `"resources": {"RESOURCE_NAME": NUM_SLOTS_NEEDED}`. If a job is not bottlenecked on a resource, it should require 0 slots. If a resource is not explicitly mentioned under the `resources` key, the job's tasks will require whatever number of slots is specified in that resource's `"default"` in `bistro_settings -> resources`.

### How does a task select the nodes, whose resources it requires?

Bistro will not start a task unless it is able to reserve the resources for the task on the nodes it uses. But what does it mean for a task to "use" a node? Let's call this the **node usage condition** -- a task `T` running on node `N_T` requires resources on a node `N` if and only if one of these holds:

 - `N` is reachable by following `parent` links from `N_T`. 
 - The scheduler assigns `T` to a worker node named `N`.

Before starting a task, Bistro will iterate over all the nodes satisfying its "node usage condition". For each node, and for each resource provided by this node (whether it has 0 available slots, or more), we will subtract the job's required number of slots from the available slots. If any node resource's "available" drops below 0 after this subtraction, the task will not be started.

There are a few important caveats to understand:

 - In all of the above, "resources" refer to logical resources -- integer slots in a per-node hash table. Physical machine resources (like CPU and RAM) can be linked to these logical resources by "automatically discovering physical worker resources" (below), but Bistro's core scheduling happens via logical slots.

 - Bistro supports real-time changes to the resource model. If available resources decrease due to a change in configuration, an already-running task will continue running -- but the UI will also report an error about this condition. If desired, it would be easy to support terminating tasks whose logical resource requirements exceed currently available logical resources.

 - The careful reader will notice that it is possible to define nodes & jobs in such a way that jobs will require resources from nodes, which do not satisfy the "node usage condition". Don't do that. The behavior is not specified, and may change in the future.

To summarize:

- Each node has an "available" resource hash table, usually set via `limit` in `bistro_settings -> resources -> LEVEL_NAME -> RESOURCE_NAME`.
- Each job has a "required" resource hash table, set either via `default` in the `bistro_settings`, or via `JOB_NAME -> resources -> RESOURCE_NAME`.
- A task requires resources on its assigned worker host, and on all nodes accessible via `parent` from the node that the task is running on (aka the "node usage condition").

## Specifying nodes via node sources
 
Bistro's way of discovering nodes is slightly complicated, but very flexible. Here is the algorithm in pseudo-code:

* Every `--nodes_{update,retry}_ms` millseconds:
    * nodes = [ ]
    * For every item in `bistro -> nodes -> node_sources`:
        * nodes = nodes + [new nodes generated by this node source]
    * Make `nodes` available for future scheduling iterations

A node sources may refer to the nodes created by preceding node sources. Two patterns are common:
- Deriving new node names from previous sources' nodes.
  (`add_time`, `range_label`)
- Setting `parent` to previous sources' nodes.
  (`add_time`, `range_label`, `script)

So the available set of nodes depends on:
- which node sources are defined,
- how they are configured,
- the order in which they are listed.

Besides specifying `node_sources`, `bistro -> nodes` must contain `"levels": ["top level", "lower level", ...]`. Bistro automatically prepends `instance` to this list, so that level name is always available. As stated in the section about nodes, a node at a certain level should have a parent at the previous level. So this list defines both the available level names, and their order.

### Open-source node sources

Bistro provides the following node sources -- peek at their code in [bistro/nodes/](https://github.com/facebookarchive/bistro/blob/main/bistro/nodes) for the configuration options. Most are super-simple, while `add_time` provides detailed documentation in a docblock.

Most node sources support a `parent_level` setting, which also implicitly sets the level at which it emits new nodes -- look for a call to `getMyLevelAndParents` in their `.cpp` files.

- `manual`: This source allows describing arbitrary node trees. It is great for not-too-large fixed workloads. See the docblock at the top of [ManualFetcher.h](https://github.com/facebookarchive/bistro/blob/main/bistro/nodes/ManualFetcher.h).
- `range_label`: Bistro's analog of the `for` loop. For each node of a parent level, makes child nodes labeled with integers `start` through `end` (the name format is customizable via `format`). Good for breaking up data into finer shards. Details in [RangeLabelFetcher.cpp](https://github.com/facebookarchive/bistro/blob/main/bistro/nodes/RangeLabelFetcher.cpp).
- `script`: For each `parent_level` node, runs `script` with the parent node name as its only argument. Each output line becomes a child node name.
- `add_time`: Given a parent level of data nodes, creates periodic work items for each of these nodes. Supports sophisticated `cron`-style scheduling, as well as simple "run every N seconds" workloads. Read [the detailed docblock for AddTimeFetcher](https://github.com/facebookarchive/bistro/blob/main/bistro/nodes/AddTimeFetcher.h#L25).

Most applications' initial needs can be satisfied with `manual`, `range_label`, and `script` sources. For periodic scheduling, other node sources may be combined with the `add_time` source. Once better performance or customization is required, it is easy to add a new node sources -- Bistro provides a simple plugin interface for this purpose. If you make something generally useful (e.g. a DB-backed workqueue), send us a patch!

## Advanced resource configuration: worker entitlements

Every job, even one lacking a `"resources": {}` setting, requires some number of slots of **every resource**, subject to the "node usage condition" above.

This allow us to implement resource domains or entitlements. There are two possible goals: to keep jobs out of an entitlement, and to confine jobs inside an entitlement.

Caveats:

 - Each additional resource incurs a small scheduling overhead, so the strategy described in this section will not scale for partitioning your available resources into 1000s of entitlements. But dozens have been tested, and work fine.
 - At present, overrides are only available for worker resources -- but this would be easy to fix if you needed to create data resource entitlements.

### Preventing regular jobs from using a special resource

If you need to prevent most jobs from running on a special set of reserved resources, add to `bistro -> resources` something like 

``` json
"avoid-my-special-resource": {"limit": 1000000, "default": 1}
```

Then set 

``` json
"worker_resources_overrides": {"RESERVED_HOST_FQDN": {"avoid-my-special-resource": 0}}
```

to prevent default jobs from running on your special hosts. 

Jobs that are permitted to run on the special resource will need to set `"resources": {"avoid-my-special-resource": 0}`. 

### Forcing special jobs to run on a special resource

If your job is only able to run on a special subset of resources, the reverse approach works. First, add to `bistro -> resources` something like

``` json
"force-my-special-resource": {"limit": 0, "default": 0}
```

This will have no effect on regular jobs. To confine your special job, first mark the special hosts via

``` json
"worker_resources_overrides": {"SPECIAL_HOST_FQDN": {"force-my-special-resource": 1000000}}
```

Then, you can mark special jobs with `"resources": {"force-my-special-resource": 1}`, which will prevent them from running on unmarked hosts.

### Partitioning resources into exclusive entitlements

Combining the two preceding ideas, you can make it so that your special jobs, and only your special jobs, run on your special resources. Just configure `avoid-my-special-resource` and `force-my-special-resource` as discussed above.

## Advanced resource configuration: automatically discovering physical worker resources

See [Physical resources: discovering and enforcing](https://facebook.github.io/bistro/docs/physical-resources/).
