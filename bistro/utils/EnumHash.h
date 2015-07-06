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
