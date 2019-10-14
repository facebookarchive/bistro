/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/physical/CGroupPaths.h"

namespace facebook { namespace bistro {

class AllTasksPhysicalResourceMonitor;

// Fetches task resources every time, no caching.
class TaskPhysicalResourceFetcher {
public:
  TaskPhysicalResourceFetcher(
    CGroupPaths cgp,
    AllTasksPhysicalResourceMonitor* res_mon  // Assumed to outlive us
  ) : cgpaths_(std::move(cgp)), allTasksMonitor_(res_mon) {}

  cpp2::TaskPhysicalResources fetch() const;

private:
  const CGroupPaths cgpaths_;
  const AllTasksPhysicalResourceMonitor* allTasksMonitor_;
};

}}  // namespace facebook::bistro
