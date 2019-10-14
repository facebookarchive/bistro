/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/iterator/iterator_facade.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/serialization/strong_typedef.hpp>
#include <folly/Synchronized.h>
#include <folly/sorted_vector_types.h>
#include <memory>
#include <string>
#include <vector>

#include "bistro/bistro/utils/SymbolTable.h"

namespace facebook { namespace bistro {

// TODO(agoder): replace with an opaque integer type
typedef int NodeLevel;

namespace detail {
  class NodeParentIterator;
}

class Node {
public:
  using TagSet = folly::sorted_vector_set<std::string>;

  BOOST_STRONG_TYPEDEF(int, ID);

  explicit Node(
    std::string name,
    int level = 0,
    bool enabled = false,
    const Node* = nullptr,
    TagSet tags = TagSet()
  );

  // move-only, no copies
  Node(Node&&) = default;
  Node& operator=(Node&&) = default;

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  boost::iterator_range<detail::NodeParentIterator> traverseUp() const;
  std::vector<std::string> getPathToNode() const;

  static folly::Synchronized<StringTable> NodeNameTable;

  inline ID id() const { return id_; }
  inline const std::string& name() const { return name_; }
  inline int level() const { return level_; }
  inline bool enabled() const { return enabled_; }
  inline const Node* parent() const { return parent_; }
  inline const TagSet& tags() const { return tags_; }

  bool hasTags(const std::vector<std::string>& tags) const;

  // TODO(9307131): This is a temporary hack, since it was a smaller code
  // change o have this offset into PackedResources ride along directly on
  // the node than to wrap them both in a struct (the right fix).  This
  // works ok, since the scheduler is single-threaded at present.
  mutable size_t offset = 0xffffffffffffffff;

private:
  friend class detail::NodeParentIterator;

  ID id_;
  std::string name_;
  int level_;
  bool enabled_;
  const Node* parent_;
  TagSet tags_;
};

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
