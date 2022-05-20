---
docid: rest-api
title: Using the REST API
layout: docs
permalink: /docs/rest-api/
---

# Using the REST API

Bistro's scheduler speaks JSON on `--http_server_port`. You can bundle several different actions / queries into one JSON request, as follows:

``` sh
curl -d '{"my_query": {"handler":"jobs", "jobs": ["TEST_JOB"]}}' host:port
```

In the above, you can add other actions as siblings of `my_query`, and they will be executed in some order by the scheduler — but there is no guarantee of atomicity.

The response will be a matching JSON hash-map with keys corresponding to your sub-queries, e.g.

``` json
{
   "my_query": {
      "data": {
         "TEST_JOB": {
            "create_time": 1486672893,
            "enabled": true,
            "owner": "your_user",
            "level_for_tasks": "your_level",
            "priority": 1337,
            "resources": {
               "tasks_per_worker": 1,
               "tasks_per_data_shard": 1
            },
            "kill_subprocess": {
               "kill_wait_ms": 0,
               "method": "term"
            },
            "config": {
               "your": "custom config"
            },
            "modify_time": 1486672893,
            "backoff": [5, 10, 20, "fail"],
            "kill_orphan_tasks_after_sec": 90,
            "task_subprocess": {
               "cgroups": {
                  "kill_without_freezer": false,
                  "subsystems": [],
                  "slice": "",
                  "root": ""
               },
               "use_canary_pipe": true,
               "process_group_leader": true,
               "parent_death_signal": 9,
               "max_log_lines_per_poll_interval": 50,
               "poll_ms": 10
            }
         }
      }
   }
}
```

To see the list of available handlers, and their behavior, take a look at [bistro/server/HTTPMonitor.cpp](https://github.com/facebookarchive/bistro/blob/main/bistro/server/HTTPMonitor.cpp).
