/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/operators.hpp>
#include <boost/regex.hpp>
#include <functional>
#include <string>
#include <unordered_set>

#include <folly/dynamic.h>
#include <folly/hash/Hash.h>

namespace facebook { namespace bistro {

class Node;

class JobFilters : boost::equality_comparable<JobFilters> {
public:
  // Returns true if the job can run on this node. An nullptr Function
  // accepts all nodes.
  using NodeDoesPassCob = std::function<bool(const Node&)>;
public:
  JobFilters();
  explicit JobFilters(const folly::dynamic& d,
    NodeDoesPassCob filter_cb = nullptr);

  JobFilters(JobFilters&&) = default;
  JobFilters(const JobFilters&) = default;
  JobFilters& operator=(JobFilters&&) = default;
  JobFilters& operator=(const JobFilters&) = default;

  /**
   * Check if a Node passes these filters.
   */
  bool doesPass(const std::string& salt, const Node& n) const;

  /**
   * Check if a string passes these filters. Unlike the overload above this does
   * not check the tag whitelist.
   */
  [[deprecated("Do not add new calls. This function will go away once "
                   "workers are Nodes, nor does it run your filter_cb")]]
  bool doesPass(const std::string& salt, const std::string& s) const;

  bool isEmpty() const;

  folly::dynamic toDynamic() const;

  bool operator==(const JobFilters&) const;

private:
  std::unordered_set<std::string> whitelist_;
  boost::regex whitelistRegex_;
  std::unordered_set<std::string> blacklist_;
  boost::regex blacklistRegex_;
  std::vector<std::string> tagWhitelist_;
  double fractionOfNodes_;
  size_t fractionCutoff_;
  bool isEmpty_;
  std::hash<std::pair<std::string, std::string>> hasher_;
  std::function<bool(const Node& n)> cb_;

  void setFractionOfNodes(double fraction);
  bool isNonTriviallyEmpty() const;

};

}}
