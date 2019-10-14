/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <thread>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/if/gen-cpp2/BistroScheduler.h"
#include "bistro/bistro/if/gen-cpp2/scheduler_types.h"
#include "common/fb303/cpp/FacebookBase2.h"
#include <folly/Synchronized.h>

namespace facebook {
namespace concurrency {
  class RepeatableThread;
}

namespace bistro {

class TaskRunner;
class ConfigLoader;
class NodesLoader;
class Scheduler;
class TaskStatuses;
class Monitor;
namespace cpp2 {
  class BistroInstanceID;
  class BistroWorker;
  class RunningTask;
}

class ThriftMonitor : public cpp2::BistroSchedulerSvIf,
                      public fb303::FacebookBase2 {

public:
  ThriftMonitor(
    std::shared_ptr<ConfigLoader> config_loader,
    std::shared_ptr<NodesLoader> nodes_loader,
    std::shared_ptr<TaskStatuses> task_statuses,
    std::shared_ptr<TaskRunner> task_runner,
    std::shared_ptr<Monitor> monitor
  );

  ~ThriftMonitor() override;

  fb303::cpp2::fb_status getStatus() override;

  void getJobs(std::vector<cpp2::BistroJobConfig>&) override;

  void getJob(cpp2::BistroJobConfig&, const std::string&) override;

  void saveJob(const cpp2::BistroJobConfig& job) override;

  void deleteJob(const std::string& job_name) override;

  void forgiveJob(const std::string& job) override;

  void getLevels(std::vector<std::string>&) override;

  void getJobHistograms(
    std::vector<cpp2::BistroJobHistogram>& histograms,
    const std::vector<std::string>& jobs,
    int samples
  ) override;

  void getJobLogsByTime(
    cpp2::LogLines& out,
    const std::string& logtype,
    const std::string& job_id,
    const std::string& node_id,
    int32_t time,
    bool is_ascending,
    const std::string& regex_filter
  ) override;

  void getJobLogsByID(
    cpp2::LogLines& out,
    const std::string& logtype,
    const std::string& job_id,
    const std::string& node_id,
    int64_t line_id,
    bool is_ascending,
    const std::string& regex_filter
  ) override;

  void processHeartbeat(
    cpp2::SchedulerHeartbeatResponse& out_response,
    const cpp2::BistroWorker&,
    const cpp2::WorkerSetID&
  ) override;

  void updateStatus(
    const cpp2::RunningTask& rt,
    const std::string& status,
    const cpp2::BistroInstanceID& scheduler_id,
    const cpp2::BistroInstanceID& worker_id
  ) override;

private:
  void runScheduler() noexcept;
  void refreshNodes() noexcept;

  void getJobLogs(  // Because unique_ptr...
    cpp2::LogLines& out,
    const std::string& logtype,
    const std::string& job_id,
    const std::string& node_id,
    int64_t line_id,
    bool is_ascending,
    const std::string& regex_filter
  );

  fb303::cpp2::fb_status status_;

  std::shared_ptr<ConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> taskStatuses_;
  std::shared_ptr<TaskRunner> taskRunner_;
  std::shared_ptr<Monitor> monitor_;
};

}}
