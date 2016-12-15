/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
