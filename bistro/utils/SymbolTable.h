/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace facebook { namespace bistro {

/**
 * Maintains a fixed mapping of some object (often string) to consecutive
 * integers starting from 0. This is useful if you have a small set of possible
 * objects that you reuse. You can identify them by small integer ids, thus
 * using a vector/array like a map.
 */

template<class T>
class SymbolTable {

public:
  const static int NotFound;

  SymbolTable() {}

  SymbolTable(std::initializer_list<T> items) : objects_(items) {
    for (int i = 0; i < objects_.size(); ++i) {
      objToIndex_[objects_[i]] = i;
    }
  }

  int lookup(const T& obj) const {
    auto it = objToIndex_.find(obj);
    if (it != objToIndex_.end()) {
      return it->second;
    }
    return NotFound;
  }

  int insert(const T& obj) {
    auto it = objToIndex_.find(obj);
    if (it != objToIndex_.end()) {
      return it->second;
    }
    int index = objects_.size();
    objects_.push_back(obj);
    objToIndex_[obj] = index;
    return index;
  }

  const T& lookup(int id) const {
    return objects_[id];
  }

  const std::vector<T>& all() const {
    return objects_;
  }

  size_t size() const {
    return objects_.size();
  }

private:
  std::unordered_map<T, int> objToIndex_;
  std::vector<T> objects_;

};

// Future: untemplate this, just call it kSymbolTableNotFound.
template<class T>
constexpr int SymbolTable<T>::NotFound = -1;

typedef SymbolTable<std::string> StringTable;

}}
