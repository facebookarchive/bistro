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
