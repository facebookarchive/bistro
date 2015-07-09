/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/config/Node.h"

#include <algorithm>
#include <string>
#include <vector>

namespace facebook { namespace bistro {

folly::Synchronized<StringTable> Node::NodeNameTable =
  folly::Synchronized<StringTable>();

using namespace std;

Node::Node(
    const string& n,
    const int l,
    const bool e,
    const Node* p,
    const unordered_set<string>& tags)
  : id_(NodeNameTable->insert(n)), name_(n), level_(l),
    enabled_(e), parent_(p), tags_(tags) {
}

boost::iterator_range<detail::NodeParentIterator> Node::traverseUp() const {
  return boost::make_iterator_range(
    detail::NodeParentIterator(this),
    detail::NodeParentIterator()
  );
}

vector<string> Node::getPathToNode() const {
  if (!parent_) {
    return vector<string>{name_};
  }
  vector<string> v(parent_->getPathToNode());
  v.push_back(name_);
  return v;
}

bool Node::hasTags(const unordered_set<string>& tags) const {
  for (const auto& t : tags) {
    if (tags_.count(t)) {
      return true;
    }
  }
  return false;
}

namespace detail {

NodeParentIterator::NodeParentIterator() : curNode_(nullptr) {}

NodeParentIterator::NodeParentIterator(const Node* n) : curNode_(n) {}

void NodeParentIterator::increment() {
  curNode_ = curNode_->parent_;
}

bool NodeParentIterator::equal(const NodeParentIterator& other) const {
  return curNode_ == other.curNode_;
}

const Node& NodeParentIterator::dereference() const {
  return *curNode_;
}

}

}}
