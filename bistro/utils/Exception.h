/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
