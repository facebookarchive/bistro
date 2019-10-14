/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <type_traits>

namespace facebook { namespace bistro {

struct EnumHash {
  template<class T>
  typename std::enable_if<std::is_enum<T>::value, std::size_t>::type
  operator()(const T value) const {
    return static_cast<std::size_t>(value);
  }
};

}}
