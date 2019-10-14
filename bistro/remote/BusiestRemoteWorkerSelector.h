/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/remote/RemoteWorkerSelector.h"

namespace facebook { namespace bistro {

class BusiestRemoteWorkerSelector : public RemoteWorkerSelector {
public:
  TaskRunnerResponse findWorker(
    const Config* config,
    const Job& job,
    const Node& node,
    int worker_level,
    Monitor* monitor,
    WorkerResources* worker_resources,
    RemoteWorkers* workers,
    cpp2::BistroWorker* found_worker,
    int64_t* did_not_run_sequence_num
  ) noexcept override;
};

}}  // namespace facebook::bistro
