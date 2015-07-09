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

#include <boost/iterator/filter_iterator.hpp>
#include <boost/noncopyable.hpp>
#include <boost/range/iterator_range.hpp>
#include <glog/logging.h>
#include <memory>
#include <vector>

#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/utils/ShuffledRange.h"

namespace facebook { namespace bistro {

namespace detail {
struct DoesNodeBelongToLevel {
  explicit DoesNodeBelongToLevel(NodeLevel level) : level_(level) {}
  bool operator()(const NodePtr& node) { return node->level() == level_; }
  NodeLevel level_;
};
}

class Nodes {

private:
  typedef boost::filter_iterator<
    detail::DoesNodeBelongToLevel,
    std::vector<NodePtr>::const_iterator
  > LevelIter;
  typedef boost::iterator_range<LevelIter> LevelIterRange;

public:

  Nodes();
  Nodes(const Nodes&) = delete;
  Nodes(Nodes&&) noexcept = default;
  Nodes& operator=(Nodes&&) = default;

  static std::string getInstanceNodeName();

  template<class... Args>
  const Node* add(Args&&... args) {
    nodes_.emplace_back(std::make_shared<Node>(std::forward<Args>(args)...));
    return nodes_.back().get();
  }

  // NB: Potentially, this could return an iterator to the inserted elements
  template <class It>
  void add(It f, It l) {
    nodes_.insert(nodes_.end(), f, l);
  }

  LevelIterRange iterateOverLevel(NodeLevel level) const;

  ShuffledRange<std::vector<NodePtr>::const_iterator> shuffled() const;

  size_t size() const {
    return nodes_.size();
  }

  const Node* getInstance() const {
    return nodes_.front().get();
  }

  // EXTREMELY INEFFICIENT; used only in unit tests.
  NodePtr getNodeVerySlow(const std::string& name) const;

  std::vector<NodePtr>::const_iterator begin() const {
    return nodes_.begin();
  }

  std::vector<NodePtr>::const_iterator end() const {
    return nodes_.end();
  }

private:
  std::vector<NodePtr> nodes_;

};

}}
