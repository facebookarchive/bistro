/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

namespace facebook { namespace bistro {

class SchedulerPolicy;

// This has to be a shared_ptr so that we don't have to know how to delete
// SchedulerPolicy.
void registerSchedulerPolicy(std::string, std::shared_ptr<SchedulerPolicy>);
void throwUnlessPolicyNameExists(const std::string&);  // appease agoder@
SchedulerPolicy* getSchedulerPolicy(const std::string&);

}}  // namespace facebook::bistro
