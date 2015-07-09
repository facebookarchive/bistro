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

#include <boost/iterator/iterator_facade.hpp>
#include <boost/noncopyable.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/strong_typedef.hpp>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "bistro/bistro/utils/SymbolTable.h"
#include <folly/Synchronized.h>

namespace facebook { namespace bistro {

// TODO(agoder): replace with an opaque integer type
typedef int NodeLevel;

namespace detail {
  class NodeParentIterator;
}

class Node : boost::noncopyable {

public:

  BOOST_STRONG_TYPEDEF(int, ID);

  explicit Node(
    const std::string& name,
    const int level = 0,
    const bool enabled = false,
    const Node* = nullptr,
    const std::unordered_set<std::string>& = std::unordered_set<std::string>()
  );

  Node(Node&&) = default;
  Node& operator=(Node&&) = default;

  boost::iterator_range<detail::NodeParentIterator> traverseUp() const;
  std::vector<std::string> getPathToNode() const;

  static folly::Synchronized<StringTable> NodeNameTable;

  inline ID id() const { return id_; }
  inline const std::string& name() const { return name_; }
  inline int level() const { return level_; }
  inline bool enabled() const { return enabled_; }
  inline const Node* parent() const { return parent_; }
  inline const std::unordered_set<std::string>& tags() const { return tags_; }

  bool hasTags(const std::unordered_set<std::string>& tags) const;

private:
  friend class detail::NodeParentIterator;

  ID id_;
  std::string name_;
  int level_;
  bool enabled_;
  const Node* parent_;
  std::unordered_set<std::string> tags_;

};

typedef std::shared_ptr<const Node> NodePtr;

namespace detail {

/**
 * Iterator that lets you traverse up the parent hierarchy from a node to the
 * root.
 * Example:
 * for (const auto& node : n.traverseUp()) {
 *   // Access node
 * }
 */
class NodeParentIterator
  : public boost::iterator_facade<
      NodeParentIterator,
      const Node,
      boost::forward_traversal_tag> {

public:
  NodeParentIterator();
  explicit NodeParentIterator(const Node* n);

private:
  friend class boost::iterator_core_access;

  bool equal(const NodeParentIterator& other) const;
  const Node& dereference() const;
  void increment();

  const Node* curNode_;

};

}

}}

namespace std {
  template<>
  struct hash<facebook::bistro::Node::ID>
  {
    size_t operator()(facebook::bistro::Node::ID id) const
    {
      return std::hash<int>()(static_cast<int>(id));
    }
  };
}
