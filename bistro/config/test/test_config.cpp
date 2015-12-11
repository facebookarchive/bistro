/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include <folly/Conv.h>
#include <folly/dynamic.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/SchedulerType.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"

using apache::thrift::debugString;
using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestConfig, HandleConstruction) {
  dynamic d = dynamic::object
    ("enabled", true)
    ("working_wait", 0.5)
    ("idle_wait", 5.5)
    ("scheduler", "ranked_priority")
    ("nodes", dynamic::object
      ("levels", { "level1" , "level2" })
      ("node_source", "range_label")
      ("node_source_prefs", dynamic::object
        ("key1", "val1")
        ("key2", "val2")
      )
      ("node_sources", {
        dynamic::object
          ("source", "tier_services")
          ("prefs", dynamic::object("key", "val")),
        dynamic::object
          ("source", "dbmaster_children")
          ("prefs", dynamic::object("key", "val")),
      })
    )
    ("resources", dynamic::object
      ("level1", dynamic::object
        ("resource_name", dynamic::object
          ("default", 1)
          ("limit", 3)
        )
      )
      ("worker", dynamic::object
        ("worker_resource_name", dynamic::object
          ("default", 5)
          ("limit", 6)
          ("weight", 3)
        )
      )
    )
    ("worker_resources_override", dynamic::object
      ("worker1:17", dynamic::object
        ("worker_resource_name", 55)
      )
      ("worker2:19", dynamic::object
        ("worker_resource_name", 66)
      )
    )
  ;

  Config c(d);
  EXPECT_TRUE(c.enabled);
  EXPECT_EQ(chrono::milliseconds(500), c.workingWait);
  EXPECT_EQ(chrono::milliseconds(5500), c.idleWait);
  EXPECT_EQ(SchedulerType::RankedPriority, c.schedulerType);

  ASSERT_EQ(3, c.nodeConfigs.size());
  EXPECT_EQ("range_label", c.nodeConfigs[0].source);
  EXPECT_EQ("val1", c.nodeConfigs[0].prefs.convert<string>("key1"));
  EXPECT_EQ("val2", c.nodeConfigs[0].prefs.convert<string>("key2"));

  EXPECT_EQ("tier_services", c.nodeConfigs[1].source);
  EXPECT_EQ("val", c.nodeConfigs[1].prefs.convert<string>("key"));

  EXPECT_EQ("dbmaster_children", c.nodeConfigs[2].source);
  EXPECT_EQ("val", c.nodeConfigs[2].prefs.convert<string>("key"));

  EXPECT_FALSE(c.killOrphanTasksAfter.hasValue());

  // Check default and non-default enums
  EXPECT_EQ(RemoteWorkerSelectorType::RoundRobin, c.remoteWorkerSelectorType);
  d["remote_worker_selector"] = "busiest";
  EXPECT_EQ(
    RemoteWorkerSelectorType::Busiest, Config(d).remoteWorkerSelectorType
  );
  EXPECT_EQ(NodeOrderType::Random, c.nodeOrderType);
  d["nodes"][kNodeOrder] = "original";
  EXPECT_EQ(NodeOrderType::Original, Config(d).nodeOrderType);

  // Check default and non-default task options
  cpp2::TaskSubprocessOptions task_opts;
  EXPECT_EQ(task_opts, c.taskSubprocessOptions);
  d[kTaskSubprocess] = folly::dynamic::object
    (kPollMs, 111)
    (kMaxLogLinesPerPollInterval, 222)
    (kParentDeathSignal, 333)
    (kProcessGroupLeader, true)
    (kUseCanaryPipe, false)
    (kCGroups, folly::dynamic::object
      (kRoot, "root")
      (kSlice, "slice")
      (kSubsystems, {"sys1", "sys2"})
    );
  task_opts.pollMs = 111;
  task_opts.maxLogLinesPerPollInterval = 222;
  task_opts.parentDeathSignal = 333;
  task_opts.processGroupLeader = true;
  task_opts.useCanaryPipe = false;
  task_opts.cgroupOptions.root = "root";
  task_opts.cgroupOptions.slice = "slice";
  task_opts.cgroupOptions.subsystems = {"sys1", "sys2"};
  EXPECT_EQ(task_opts, Config(d).taskSubprocessOptions);

  // Check non-default physical resource configs
  cpp2::PhysicalResourceConfigs prcs;
  EXPECT_EQ(prcs, c.physicalResourceConfigs);
  d[kPhysicalResources] = folly::dynamic::object
    (kRamMB, folly::dynamic::object
      (kLogicalResource, "my_ram_gb")
      (kMultiplyLogicalBy, 1024)
      (kEnforcement, kHard)
    )
    (kCPUCore, folly::dynamic::object
      (kLogicalResource, "my_cpu_centicore")
      (kMultiplyLogicalBy, 0.001)
      (kEnforcement, kSoft)
    )
    (kGPUCard, folly::dynamic::object
      (kLogicalResource, "my_gpu_card")
      (kMultiplyLogicalBy, 1)
      (kEnforcement, kNone)
    );
  auto prc_it = prcs.configs.emplace(
    cpp2::PhysicalResource::RAM_MBYTES, cpp2::PhysicalResourceConfig()
  ).first;
  prc_it->second.physical = cpp2::PhysicalResource::RAM_MBYTES;
  prc_it->second.logical = "my_ram_gb";
  prc_it->second.multiplyLogicalBy = 1024;
  prc_it->second.enforcement = cpp2::PhysicalResourceEnforcement::HARD;
  prc_it = prcs.configs.emplace(
    cpp2::PhysicalResource::CPU_CORES, cpp2::PhysicalResourceConfig()
  ).first;
  prc_it->second.physical = cpp2::PhysicalResource::CPU_CORES;
  prc_it->second.logical = "my_cpu_centicore";
  prc_it->second.multiplyLogicalBy = 0.001;
  prc_it->second.enforcement = cpp2::PhysicalResourceEnforcement::SOFT;
  prc_it = prcs.configs.emplace(
    cpp2::PhysicalResource::GPU_CARDS, cpp2::PhysicalResourceConfig()
  ).first;
  prc_it->second.physical = cpp2::PhysicalResource::GPU_CARDS;
  prc_it->second.logical = "my_gpu_card";
  prc_it->second.multiplyLogicalBy = 1;
  prc_it->second.enforcement = cpp2::PhysicalResourceEnforcement::NONE;
  {
    Config config(d);
    const auto& p = config.physicalResourceConfigs;
    EXPECT_EQ(prcs, p) << debugString(prcs) << " != " << debugString(p);
    EXPECT_EQ(((decltype(config.logicalToPhysical)){
      {"my_ram_gb", &p.configs.at(cpp2::PhysicalResource::RAM_MBYTES)},
      {"my_cpu_centicore", &p.configs.at(cpp2::PhysicalResource::CPU_CORES)},
      {"my_gpu_card", &p.configs.at(cpp2::PhysicalResource::GPU_CARDS)},
    }), config.logicalToPhysical);
  }

  cpp2::KillRequest kill_req;
  EXPECT_EQ(kill_req, c.killRequest);
  // Does the Thrift enum have a sane default?
  EXPECT_EQ(cpp2::KillMethod::TERM, c.killRequest.method);

  // Non-default kill request
  d[kKillSubprocess] =
    folly::dynamic::object(kMethod, kKill)(kKillWaitMs, 987);
  kill_req.method = cpp2::KillMethod::KILL;
  kill_req.killWaitMs = 987;
  EXPECT_EQ(kill_req, Config(d).killRequest);

  // levelForTasks defaults to the bottom (non-worker) level
  EXPECT_EQ(2, c.levelForTasks);
  d["level_for_tasks"] = "level2";
  EXPECT_EQ(2, Config(d).levelForTasks);
  d["level_for_tasks"] = "level1";
  EXPECT_EQ(1, Config(d).levelForTasks);
  d["level_for_tasks"] = "instance";  // The instance level is also fine
  EXPECT_EQ(0, Config(d).levelForTasks);
  d["level_for_tasks"] = "chicken";  // Throw on invalid levels
  EXPECT_THROW({Config _c(d);}, runtime_error);
  d.erase("level_for_tasks");  // Valid again

  {
    d["kill_orphan_tasks_after_sec"] = 0;
    EXPECT_EQ(0, Config{d}.killOrphanTasksAfter.value().count());
  }

  {
    const int idx = c.resourceNames.lookup("worker_resource_name");
    EXPECT_EQ(55, c.workerResourcesOverride["worker1:17"][idx]);
    EXPECT_EQ(66, c.workerResourcesOverride["worker2:19"][idx]);
    EXPECT_EQ(6, c.resourcesByLevel[c.levels.lookup("worker")][idx]);
    EXPECT_EQ(5, c.defaultJobResources[idx]);
    EXPECT_EQ(3, c.resourceIDToWeight[idx]);
  }

  {
    const int idx = c.resourceNames.lookup("resource_name");
    EXPECT_EQ(3, c.resourcesByLevel[c.levels.lookup("level1")][idx]);
    EXPECT_EQ(1, c.defaultJobResources[idx]);
    EXPECT_EQ(0, c.resourceIDToWeight[idx]);
  }

  d["worker_resources_override"]["worker5:55"] = dynamic::object
    ("invalid_resource", 123)
  ;
  EXPECT_THROW({Config _c(d);}, runtime_error);

  d["worker_resources_override"]["worker5:55"] = dynamic::object
    ("resource_name", 123) // Not a worker level resource
  ;
  EXPECT_THROW({Config _c(d);}, runtime_error);
}

TEST(TestConfig, TestMissingData) {
  EXPECT_THROW(Config(dynamic(dynamic::object)), runtime_error);
  EXPECT_THROW(
    Config(dynamic::object("nodes", dynamic::object)),
    runtime_error
  );
  EXPECT_THROW(
    Config(dynamic::object
      ("nodes", dynamic::object
       ("levels", { "foo" })
      )
    ),
    runtime_error
  );
}

TEST(TestConfig, TestInvalidLevel) {
  EXPECT_THROW(
    Config(dynamic::object
      ("nodes", dynamic::object
        ("levels", { "foo" })
      )
      ("resources", dynamic::object
        ("invalid_level", dynamic::object
          ("foo_resource", dynamic::object)
        )
      )
    ),
    runtime_error
  );
}
