/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/scheduler/SchedulerPolicyRegistry.h"

#include <glog/logging.h>
#include <unordered_map>

#include "bistro/bistro/utils/Exception.h"

namespace facebook { namespace bistro {

namespace {
std::unordered_map<std::string, std::shared_ptr<SchedulerPolicy>> policies;
}  // anonymous namespace

void registerSchedulerPolicy(
    std::string name,
    std::shared_ptr<SchedulerPolicy> policy) {
  auto p = policies.emplace(std::move(name), std::move(policy));
  CHECK(p.second) << "scheduler policy " << p.first->first
    << " is already registered";
}

namespace {
decltype(policies)::iterator findPolicyOrThrow(const std::string& name) {
  auto it = policies.find(name);
  if (it == policies.end()) {
    throw BistroException(
      "scheduler policy ", name, " is not registered");
  }
  return it;
}
}  // anonymous namespace

void throwUnlessPolicyNameExists(const std::string& name) {
  findPolicyOrThrow(name);
}

SchedulerPolicy* getSchedulerPolicy(const std::string& name) {
  return findPolicyOrThrow(name)->second.get();
}

}}  // namespace facebook::bistro
