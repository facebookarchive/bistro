#pragma once

#include <stdexcept>
#include <string>

#include "folly/Conv.h"

namespace facebook { namespace bistro {

template<typename... Args>
std::runtime_error BistroException(Args&&... args) {
  return
    std::runtime_error(folly::to<std::string>(std::forward<Args>(args)...));
}

}}
