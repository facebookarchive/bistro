/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/Conv.h>
#include <folly/dynamic.h>
#include <thrift/lib/cpp2/protocol/DebugProtocol.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/parsing_common.h"
#include "bistro/bistro/scheduler/SchedulerPolicyRegistry.h"
#include "bistro/bistro/if/gen-cpp2/common_types_custom_protocol.h"

using apache::thrift::debugString;
using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestConfig, HandleConstruction) {
  registerSchedulerPolicy(kSchedulePolicyRankedPriority.str(), nullptr);

  dynamic d = dynamic::object
    (kEnabled, true)
    ("working_wait", 0.5)
    ("idle_wait", 5.5)
    ("scheduler", kSchedulePolicyRankedPriority)
    (kNodes, dynamic::object
      ("levels", dynamic::array("level1" , "level2"))
      ("node_sources", dynamic::array(
        dynamic::object
          ("source", "range_label")
          ("prefs", dynamic::object("key1", "val1")("key2", "val2")),
        dynamic::object
          ("source", "tier_services")
          ("prefs", dynamic::object("key", "val")),
        dynamic::object
          ("source", "dbmaster_children")
          ("prefs", dynamic::object("key", "val"))
      ))
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
        // These are required for the logical-physical mapping.  They also
        // lack overrides and so must not be in workerResourcesOverride.
        ("my_ram_gb", dynamic::object("default", 2)("limit", 30))
        ("my_cpu_centicore", dynamic::object("default", 100)("limit", 700))
        ("my_gpu_card", dynamic::object("default", 1)("limit", 5))
      )
    )
    (kWorkerResourceOverride, dynamic::object
      ("worker1:17", dynamic::object("worker_resource_name", 55))
      ("worker2:19", dynamic::object("worker_resource_name", 66))
    )
    (kExitInitialWaitBeforeTimestamp, 123)
  ;

  Config c(d);
  EXPECT_TRUE(c.enabled);
  EXPECT_EQ(std::chrono::milliseconds(500), c.workingWait);
  EXPECT_EQ(std::chrono::milliseconds(5500), c.idleWait);
  EXPECT_EQ(kSchedulePolicyRankedPriority, c.schedulerPolicyName);

  // Check that we throw on invalid policy names
  {
    auto d2 = d;
    d2["scheduler"] = "not a real scheduler policy";
    EXPECT_THROW({Config _c(d2);}, std::runtime_error);
  }

  ASSERT_EQ(3, c.nodeConfigs.size());
  EXPECT_EQ("range_label", c.nodeConfigs[0].source);
  EXPECT_EQ("val1", c.nodeConfigs[0].prefs.convert<string>("key1"));
  EXPECT_EQ("val2", c.nodeConfigs[0].prefs.convert<string>("key2"));

  EXPECT_EQ("tier_services", c.nodeConfigs[1].source);
  EXPECT_EQ("val", c.nodeConfigs[1].prefs.convert<string>("key"));

  EXPECT_EQ("dbmaster_children", c.nodeConfigs[2].source);
  EXPECT_EQ("val", c.nodeConfigs[2].prefs.convert<string>("key"));

  EXPECT_FALSE(c.killOrphanTasksAfter.has_value());
  EXPECT_EQ(123, c.exitInitialWaitBeforeTimestamp);

  // Check default and non-default enums
  EXPECT_EQ(RemoteWorkerSelectorType::RoundRobin, c.remoteWorkerSelectorType);
  d["remote_worker_selector"] = "busiest";
  EXPECT_EQ(
    RemoteWorkerSelectorType::Busiest, Config(d).remoteWorkerSelectorType
  );
  EXPECT_EQ(NodeOrderType::Random, c.nodeOrderType);
  d[kNodes][kNodeOrder] = "original";
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
      (kSubsystems, dynamic::array("sys1", "sys2"))
      (kKillWithoutFreezer, true)
    );
  task_opts.pollMs = 111;
  task_opts.maxLogLinesPerPollInterval = 222;
  task_opts.parentDeathSignal = 333;
  task_opts.processGroupLeader = true;
  task_opts.useCanaryPipe = false;
  task_opts.cgroupOptions.root = "root";
  task_opts.cgroupOptions.slice = "slice";
  task_opts.cgroupOptions.subsystems = {"sys1", "sys2"};
  task_opts.cgroupOptions.killWithoutFreezer = true;
  EXPECT_EQ(task_opts, Config(d).taskSubprocessOptions);

  // Check toDynamic here, since it's easy to forget to update test_job.cpp
  {
    auto d2 = d;
    d2[kTaskSubprocess] =
      taskSubprocessOptionsToDynamic(Config(d).taskSubprocessOptions);
    EXPECT_EQ(task_opts, Config(d2).taskSubprocessOptions);
  }

  // Check default & non-default physical resource configs
  EXPECT_EQ(
    std::vector<cpp2::PhysicalResourceConfig>{},
    c.physicalResourceConfigs
  );
  d[kPhysicalResources] = folly::dynamic::object
    (kRamMB, folly::dynamic::object
      (kLogicalResource, "my_ram_gb")
      (kMultiplyLogicalBy, 1024)
      (kEnforcement, kHard)
      (kPhysicalReserveAmount, 1536)  // MB
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
  {
    Config config(d);

    cpp2::PhysicalResourceConfig ram_prc;
    ram_prc.physical = cpp2::PhysicalResource::RAM_MBYTES;
    ram_prc.logical = "my_ram_gb";
    ram_prc.logicalResourceID = config.resourceNames.lookup("my_ram_gb");
    ram_prc.multiplyLogicalBy = 1024;
    ram_prc.enforcement = cpp2::PhysicalResourceEnforcement::HARD;
    ram_prc.physicalReserveAmount = 1536;  // MB

    cpp2::PhysicalResourceConfig cpu_prc;
    cpu_prc.physical = cpp2::PhysicalResource::CPU_CORES;
    cpu_prc.logical = "my_cpu_centicore";
    cpu_prc.logicalResourceID =
      config.resourceNames.lookup("my_cpu_centicore");
    cpu_prc.multiplyLogicalBy = 0.001;
    cpu_prc.enforcement = cpp2::PhysicalResourceEnforcement::SOFT;

    cpp2::PhysicalResourceConfig gpu_prc;
    gpu_prc.physical = cpp2::PhysicalResource::GPU_CARDS;
    gpu_prc.logical = "my_gpu_card";
    gpu_prc.logicalResourceID = config.resourceNames.lookup("my_gpu_card");
    gpu_prc.multiplyLogicalBy = 1;
    gpu_prc.enforcement = cpp2::PhysicalResourceEnforcement::NONE;

    // We don't know what order physicalResourceConfigs will have.
    ASSERT_EQ(3, config.physicalResourceConfigs.size());
    ASSERT_EQ(3, config.logicalToPhysical.size());
    const auto& prcs = config.physicalResourceConfigs;
    const auto l2p = config.logicalToPhysical;
    EXPECT_EQ(ram_prc, prcs.at(l2p.at("my_ram_gb")));
    EXPECT_EQ(cpu_prc, prcs.at(l2p.at("my_cpu_centicore")));
    EXPECT_EQ(gpu_prc, prcs.at(l2p.at("my_gpu_card")));
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
  d[kLevelForTasks] = "level2";
  EXPECT_EQ(2, Config(d).levelForTasks);
  d[kLevelForTasks] = "level1";
  EXPECT_EQ(1, Config(d).levelForTasks);
  d[kLevelForTasks] = "instance";  // The instance level is also fine
  EXPECT_EQ(0, Config(d).levelForTasks);
  d[kLevelForTasks] = "chicken";  // Throw on invalid levels
  EXPECT_THROW({Config _c(d);}, runtime_error);
  d.erase(kLevelForTasks);  // Valid again

  {
    d[kKillOrphanTasksAfterSec] = 0;
    EXPECT_EQ(0, Config{d}.killOrphanTasksAfter.value().count());
  }

  {
    const int idx = c.resourceNames.lookup("worker_resource_name");
    EXPECT_EQ(
      (decltype(c.workerResourcesOverride)::mapped_type{{idx, 55}}),
      c.workerResourcesOverride["worker1:17"]
    );
    EXPECT_EQ(
      (decltype(c.workerResourcesOverride)::mapped_type{{idx, 66}}),
      c.workerResourcesOverride["worker2:19"]
    );
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

  d[kWorkerResourceOverride]["worker5:55"] = dynamic::object
    ("invalid_resource", 123)
  ;
  EXPECT_THROW({Config _c(d);}, runtime_error);

  d[kWorkerResourceOverride]["worker5:55"] = dynamic::object
    ("resource_name", 123) // Not a worker level resource
  ;
  EXPECT_THROW({Config _c(d);}, runtime_error);
}

TEST(TestConfig, TestMissingData) {
  EXPECT_THROW(Config(dynamic(dynamic::object)), runtime_error);
  EXPECT_THROW(
    Config(dynamic::object(kNodes, dynamic::object)),
    runtime_error
  );
  EXPECT_THROW(
    Config(dynamic::object
      (kNodes, dynamic::object("levels", dynamic::array("foo")))
    ),
    runtime_error
  );
}

TEST(TestConfig, TestInvalidLevel) {
  EXPECT_THROW(
    Config(dynamic::object
      (kNodes, dynamic::object("levels", dynamic::array("foo")))
      ("resources", dynamic::object
        ("invalid_level", dynamic::object
          ("foo_resource", dynamic::object)
        )
      )
    ),
    runtime_error
  );
}
