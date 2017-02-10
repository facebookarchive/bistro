/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <stdexcept>
#include <string>

#include <folly/Conv.h>

namespace facebook { namespace bistro {

template<typename... Args>
std::runtime_error BistroException(Args&&... args) {
  return
    std::runtime_error(folly::to<std::string>(std::forward<Args>(args)...));
}

std::string strError();

}}
