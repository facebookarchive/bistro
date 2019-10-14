/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <chrono>
#include <map>
#include <memory>

namespace facebook { namespace bistro {

class ConfigLoader;
class Monitor;
class NodesLoader;
class TaskStatuses;
class TaskRunner;

class Bistro : boost::noncopyable {

public:
  Bistro(
    std::shared_ptr<ConfigLoader> config_loader,
    std::shared_ptr<NodesLoader> nodes_loader,
    std::shared_ptr<TaskStatuses> task_statuses,
    std::shared_ptr<TaskRunner> task_runner,
    std::shared_ptr<Monitor> monitor
  );

  std::chrono::milliseconds scheduleOnce(std::chrono::milliseconds) noexcept;
  std::chrono::milliseconds scheduleOnceSystemTime() {
    using namespace std::chrono;
    return scheduleOnce(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
    );
  }

private:
  std::shared_ptr<ConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> taskStatuses_;
  std::shared_ptr<TaskRunner> taskRunner_;
  std::shared_ptr<Monitor> monitor_;
  // Would be an unordered_map, but I couldn't be bothered to hash pairs.
  std::map<
    std::pair<std::string, std::string>,  /*job, node*/
    std::chrono::milliseconds  /*time since epoch*/
  > orphanTaskIDToKillTime_;
};

}}
