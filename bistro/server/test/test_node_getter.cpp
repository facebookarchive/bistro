/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>
#include <stdexcept>
#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/server/HTTPMonitor.h"
#include "bistro/bistro/test/MockBistro.h"

DECLARE_string(instance_node_name);
DECLARE_int32(http_server_port);

using namespace facebook::bistro;
using folly::dynamic;

const dynamic kHostConcurrency = dynamic::object(
  "host_concurrency",
  dynamic::object("default", 1)("limit", 2)
);
const dynamic kDBConcurrency = dynamic::object(
  "db_concurrency",
  dynamic::object("default", 1)("limit", 1)("weight", 3)
);

namespace {

class GetterTest : public ::testing::Test {
 protected:
  GetterTest() {
    FLAGS_instance_node_name = "instance";
    FLAGS_http_server_port = 0;

    dynamic c = dynamic::object
      ("nodes", dynamic::object
        ("levels", dynamic::array("host", "db"))
        ("node_sources", dynamic::array(
          dynamic::object
            ("source", "manual")
            ("prefs", dynamic::object
              ("host1",
                dynamic::object("children", dynamic::array("host1.1")))
              ("host1.1", dynamic::object("disabled", true))
            )
        ))
      )
      ("resources", dynamic::object
        ("host", kHostConcurrency)
        ("db", kDBConcurrency)
      );

    config_ = std::make_shared<Config>(c);

    configLoader_ = std::make_shared<InMemoryConfigLoader>(*config_);
    nodesLoader_ = std::make_shared<NodesLoader>(
      configLoader_, std::chrono::seconds(3600), std::chrono::seconds(3600)
    );
    taskStatuses_ = std::make_shared<TaskStatuses>(
      std::make_shared<BitBucketTaskStore>()
    );

    monitor_ = std::make_shared<HTTPMonitor>(
      configLoader_, nodesLoader_, taskStatuses_,
      std::make_shared<MockRunner>(),
      std::make_shared<Monitor>(
        configLoader_, nodesLoader_, taskStatuses_
      )
    );
  }

  std::shared_ptr<HTTPMonitor> monitor_;
  std::shared_ptr<Config> config_;
  std::shared_ptr<InMemoryConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> taskStatuses_;
};


TEST_F(GetterTest, GetEverythingEveryone) {
  dynamic expected = dynamic::object("results",
    dynamic::object
    ("instance", dynamic::object
      ("instance", dynamic::object("resources", dynamic::object())))
    ("host", dynamic::object
      ("host1", dynamic::object("resources", kHostConcurrency)))
    ("db", dynamic::object
      ("host1.1", dynamic::object(
        "resources", kDBConcurrency)("disabled", true)
      ))
  );

  dynamic request = dynamic::object;
  dynamic res = monitor_->handleNodes(*config_, request);
  EXPECT_EQ(expected, res);
}

TEST_F(GetterTest, GetEverythingForOne) {
  dynamic expected = dynamic::object("results",
    dynamic::object
      ("host", dynamic::object
        ("host1", dynamic::object("resources", kHostConcurrency)))
  );
  dynamic request = dynamic::object
    ("nodes", dynamic::array("host1"))("fields", dynamic::object());

  dynamic res = monitor_->handleNodes(*config_, request);
  EXPECT_EQ(expected, res);
}

TEST_F(GetterTest, GetEverythingForOneOne) {
  dynamic expected = dynamic::object("results",
    dynamic::object
    ("db", dynamic::object
      ("host1.1", dynamic::object(
        "resources", kDBConcurrency)("disabled", true)
      ))
  );

  dynamic request = dynamic::object
    ("nodes", dynamic::array("host1.1"))("fields", dynamic::object());

  dynamic res = monitor_->handleNodes(*config_, request);
  EXPECT_EQ(expected, res);
}

TEST_F(GetterTest, DisabledWhenHealthy) {
  // Test disabled state when healthy
  dynamic expected = dynamic::object("results",
    dynamic::object("host", dynamic::object("host1", dynamic::object()))
  );

  dynamic request = dynamic::object
    ("nodes", dynamic::array("host1"))
    ("fields", dynamic::array("disabled"));

  dynamic res = monitor_->handleNodes(*config_, request);
  EXPECT_EQ(expected, res);
}

TEST_F(GetterTest, DisabledWhenDisabled) {
  // Test disabled state when disabled
  dynamic expected = dynamic::object("results",
    dynamic::object
    ("db", dynamic::object
      ("host1.1", dynamic::object("disabled", true)))
  );
  dynamic request = dynamic::object
    ("nodes", dynamic::array("host1.1"))
    ("fields", dynamic::array("disabled"));

  dynamic res = monitor_->handleNodes(*config_, request);
  EXPECT_EQ(expected, res);
}

TEST_F(GetterTest, ThrowOnUnknownField) {
  dynamic request = dynamic::object
    ("nodes", dynamic::array("host1"))
    ("fields", dynamic::array("UnkownField"));
  EXPECT_THROW(monitor_->handleNodes(*config_, request), std::runtime_error);
}

} // anonymous namespace
