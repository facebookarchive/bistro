/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    std::string n,
    int l,
    bool e,
    const Node* p,
    Node::TagSet tags)
  : id_(NodeNameTable->insert(n)), name_(std::move(n)), level_(l),
    enabled_(e), parent_(p), tags_(std::move(tags)) {
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
  v.emplace_back(name_);
  return v;
}

bool Node::hasTags(const vector<string>& tags) const {
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
