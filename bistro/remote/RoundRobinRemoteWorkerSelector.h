/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "bistro/bistro/remote/RemoteWorkerSelector.h"

namespace facebook { namespace bistro {

class RoundRobinRemoteWorkerSelector : public RemoteWorkerSelector {
public:
  TaskRunnerResponse findWorker(
    const Config*,
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
