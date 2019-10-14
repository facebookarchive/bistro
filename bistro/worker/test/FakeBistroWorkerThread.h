/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>
#include "bistro/bistro/if/gen-cpp2/BistroWorker.h"

namespace facebook { namespace bistro {

struct FakeBistroWorker : public virtual cpp2::BistroWorkerSvIf {
  using TaskSubprocessOptsCob = std::function<
    void(const cpp2::RunningTask&, const cpp2::TaskSubprocessOptions&
  )>;  // Needs RunningTask to detect healthchecks

  explicit FakeBistroWorker(TaskSubprocessOptsCob tso_cob)
    : taskSubprocessOptsCob_(std::move(tso_cob)) {}
  ~FakeBistroWorker() override {}

  void async_tm_runTask(
    std::unique_ptr<apache::thrift::HandlerCallback<void>> cb,
    const cpp2::RunningTask& rt,
    const std::string& config,
    const std::vector<std::string>& command,
    const cpp2::BistroInstanceID& scheduler,
    const cpp2::BistroInstanceID& worker,
    int64_t notify_if_tasks_not_running_sequence_num,
    const cpp2::TaskSubprocessOptions&
  ) override;

  void async_tm_getRunningTasks(
    std::unique_ptr<
      apache::thrift::HandlerCallback<std::vector<cpp2::RunningTask>>> cb,
    const cpp2::BistroInstanceID& worker
  ) override;

  TaskSubprocessOptsCob taskSubprocessOptsCob_;
};

namespace detail {
struct NoOpTaskSubprocessOptsCob {
  void operator()(
    const cpp2::RunningTask&, const cpp2::TaskSubprocessOptions&
  ) {}
};
}  // namespace detail

class FakeBistroWorkerThread {
public:
  using CustomizeWorkerCob = std::function<void(cpp2::BistroWorker*)>;

  explicit FakeBistroWorkerThread(
    std::string shard,
    CustomizeWorkerCob customize_worker_cob,
    FakeBistroWorker::TaskSubprocessOptsCob tso_cob =
      detail::NoOpTaskSubprocessOptsCob()
  ) : shard_(std::move(shard)),
      customizeWorkerCob_(std::move(customize_worker_cob)),
      ssit_(std::make_shared<FakeBistroWorker>(std::move(tso_cob))) {
  }

  cpp2::BistroWorker getBistroWorker() const;

  const std::string& shard() const { return shard_; }

private:
  std::string shard_;
  CustomizeWorkerCob customizeWorkerCob_;
  apache::thrift::ScopedServerInterfaceThread ssit_;
};

}}
