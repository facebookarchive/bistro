/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/ConfigLoader.h"

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/utils/Exception.h"
#include <folly/dynamic.h>
#include <folly/json.h>

namespace facebook { namespace bistro {

void ConfigLoader::saveJob(
    const std::string& job_name,
    const folly::dynamic& job_dyn) {

  // The ConfigLoader, not the caller, dictates the created/modified times.
  folly::dynamic d = job_dyn;
  d.erase("create_time");
  d.erase("modify_time");

  const auto c = getDataOrThrow();
  const auto& jobs = c->jobs;
  auto it = jobs.find(job_name);
  if (it == jobs.end()) {
    LOG(INFO) << "Saving new job '" << job_name << "': " << folly::toJson(d);
  } else {
    // Logging the job we are overwriting should help recover from errors.
    LOG(INFO) << "Overwriting existing job '" << job_name << "': "
      << folly::toJson(it->second->toDynamic(*c))
      << " with " << folly::toJson(d);
  }
  saveJobImpl(job_name, d);
}

void ConfigLoader::deleteJob(const std::string& job_name) {
  const auto c = getDataOrThrow();
  const auto& jobs = c->jobs;
  auto it = jobs.find(job_name);
  if (it == jobs.end()) {
    // The ConfigLoader is the authority on whether this job can be deleted
    // (i.e.  it may not have fetched it yet).
    LOG(WARNING) << "Attempting to delete a unknown job: " << job_name;
  } else {
    // Logging full pre-deletion state should help recover from errors
    LOG(INFO) << "Deleting job '" << job_name << "': "
      << folly::toJson(it->second->toDynamic(*c));
  }
  deleteJobImpl(job_name);
}

}}  // namespace facebook::bistro
