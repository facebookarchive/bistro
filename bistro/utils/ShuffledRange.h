/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <boost/iterator/indirect_iterator.hpp>
#include <iterator>
#include <numeric>
#include <folly/Random.h>


namespace facebook { namespace bistro {

template<class It>
class ShuffledRange {

  typedef boost::indirect_iterator<typename std::vector<It>::iterator> BoostIt;

public:
  ShuffledRange(It begin, It end)
    : order_(std::distance(begin, end)) {

    std::iota(order_.begin(), order_.end(), begin);
    std::shuffle(order_.begin(), order_.end(), folly::ThreadLocalPRNG());
  }

  ShuffledRange(ShuffledRange&&) = default;
  ShuffledRange& operator=(ShuffledRange&&) = default;

  BoostIt begin() {
    return BoostIt(order_.begin());
  }

  BoostIt end() {
    return BoostIt(order_.end());
  }

private:
  std::vector<It> order_;

};

template<class It>
ShuffledRange<It> shuffled(It begin, It end) {
  return ShuffledRange<It>(begin, end);
}

template<class Container>
ShuffledRange<typename Container::iterator> shuffled(Container& c) {
  return ShuffledRange<typename Container::iterator>(c.begin(), c.end());
}

template<class Container>
ShuffledRange<typename Container::const_iterator> shuffled(const Container& c) {
  return ShuffledRange<typename Container::const_iterator>(c.begin(), c.end());
}

}}
