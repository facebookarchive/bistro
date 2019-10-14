/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

#include "bistro/bistro/utils/Exception.h"
#include <folly/dynamic.h>
#include <folly/DynamicConverter.h>

namespace facebook { namespace bistro {

class SettingsMap {

public:
  explicit SettingsMap(const folly::dynamic& d = folly::dynamic::object);

  folly::dynamic get(
      const std::string& key,
      const folly::dynamic& def = folly::dynamic::object) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return def;
    }
    return it->second;
  }

  folly::dynamic require(const std::string& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      throw BistroException(key, " not found");
    }
    return it->second;
  }

  template<class T>
  T convert(const std::string& key, T def = T()) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return def;
    }
    return folly::convertTo<T>(it->second);
  }

  template<class T>
  T requireConvert(const std::string& key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      throw BistroException(key, " not found");
    }
    return folly::convertTo<T>(it->second);
  }

  void set(const std::string& key, const folly::dynamic& value) {
    map_.emplace(key, value);
  }

  std::unordered_map<std::string, folly::dynamic>::const_iterator begin() const
  {
    return map_.begin();
  }

  std::unordered_map<std::string, folly::dynamic>::const_iterator end() const {
    return map_.end();
  }

private:
  std::unordered_map<std::string, folly::dynamic> map_;

};

}}
