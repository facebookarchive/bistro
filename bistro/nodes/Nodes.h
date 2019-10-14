/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/iterator/filter_iterator.hpp>
#include <boost/noncopyable.hpp>
#include <boost/range/iterator_range.hpp>
#include <glog/logging.h>
#include <memory>
#include <vector>

#include "bistro/bistro/config/Node.h"

namespace facebook { namespace bistro {

namespace detail {
struct DoesNodeBelongToLevel {
  explicit DoesNodeBelongToLevel(NodeLevel level) : level_(level) {}
  bool operator()(const std::shared_ptr<const Node>& node) const {
    return node->level() == level_;
  }
  NodeLevel level_;
};
}

class Nodes {
private:
  using LevelIter = boost::filter_iterator<
    detail::DoesNodeBelongToLevel,
    std::vector<std::shared_ptr<const Node>>::const_iterator
  >;
  using LevelIterRange = boost::iterator_range<LevelIter>;

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

  size_t size() const {
    return nodes_.size();
  }

  const Node* getInstance() const {
    return nodes_.front().get();
  }

  std::vector<std::shared_ptr<const Node>>::const_iterator begin() const {
    return nodes_.begin();
  }

  std::vector<std::shared_ptr<const Node>>::const_iterator end() const {
    return nodes_.end();
  }

private:
  std::vector<std::shared_ptr<const Node>> nodes_;
};

}}
