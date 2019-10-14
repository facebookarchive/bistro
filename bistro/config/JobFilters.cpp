/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/JobFilters.h"

#include <limits>

#include "bistro/bistro/config/Node.h"
#include <folly/dynamic.h>

namespace facebook { namespace bistro{

using namespace folly;
using namespace std;

JobFilters::JobFilters() : isEmpty_(true) {
}

JobFilters::JobFilters(const folly::dynamic& d,
    JobFilters::NodeDoesPassCob filter_cb) : cb_(filter_cb) {
  auto jt = d.find("whitelist");
  if (jt != d.items().end()) {
    for (const auto& item : jt->second) {
      whitelist_.insert(item.asString());
    }
  }

  jt = d.find("whitelist_regex");
  if (jt != d.items().end()) {
    whitelistRegex_ = boost::regex(jt->second.asString());
  }

  jt = d.find("blacklist");
  if (jt != d.items().end()) {
    for (const auto& item : jt->second) {
      blacklist_.insert(item.asString());
    }
  }

  jt = d.find("blacklist_regex");
  if (jt != d.items().end()) {
    blacklistRegex_ = boost::regex(jt->second.asString());
  }

  jt = d.find("tag_whitelist");
  if (jt != d.items().end()) {
    for (const auto& item : jt->second) {
      tagWhitelist_.emplace_back(item.asString());
    }
  }

  setFractionOfNodes(d.getDefault("fraction_of_nodes", 1.0).asDouble());

  isEmpty_ = isNonTriviallyEmpty();
}

dynamic JobFilters::toDynamic() const {
  dynamic ret = dynamic::object;
  if (!isEmpty_) {
    if (!whitelist_.empty()) {
      ret["whitelist"] = dynamic(whitelist_.begin(), whitelist_.end());
    }
    if (!whitelistRegex_.empty()) {
      ret["whitelist_regex"] = whitelistRegex_.str();
    }
    if (!blacklist_.empty()) {
      ret["blacklist"] = dynamic(blacklist_.begin(), blacklist_.end());
    }
    if (!blacklistRegex_.empty()) {
      ret["blacklist_regex"] = blacklistRegex_.str();
    }
    if (!tagWhitelist_.empty()) {
      ret["tag_whitelist"] = dynamic(
        tagWhitelist_.begin(),
        tagWhitelist_.end()
      );
    }
    if (fractionOfNodes_ < 1.0) {
      ret["fraction_of_nodes"] = fractionOfNodes_;
    }
  }
  return ret;
}

bool JobFilters::isEmpty() const {
  return isEmpty_;
}

bool JobFilters::doesPass(const string& salt, const Node& n) const {
  return
    doesPass(salt, n.name())
      && (tagWhitelist_.empty() || n.hasTags(tagWhitelist_))
      && (!cb_ || cb_(n));
}

bool JobFilters::doesPass(const string& salt, const string& s) const {
  return isEmpty_ || (
    (whitelist_.empty() || whitelist_.count(s))
    && (whitelistRegex_.empty() || boost::regex_search(s, whitelistRegex_))
    && (blacklist_.empty() || !blacklist_.count(s))
    && (blacklistRegex_.empty() || !boost::regex_search(s, blacklistRegex_))
    && (fractionOfNodes_ == 1.0
        || hasher_(make_pair(s, salt)) < fractionCutoff_)
  );
}

bool JobFilters::operator==(const JobFilters& other) const {
  if (isEmpty_ && other.isEmpty_) {
    return true;
  }
  return
    whitelist_ == other.whitelist_
    && whitelistRegex_ == other.whitelistRegex_
    && blacklist_ == other.blacklist_
    && blacklistRegex_ == other.blacklistRegex_
    && tagWhitelist_ == other.tagWhitelist_
    && fractionOfNodes_ == other.fractionOfNodes_
    && cb_ == nullptr
    && other.cb_ == nullptr;
}

void JobFilters::setFractionOfNodes(double f)
// TODO: T26311162 fix float-cast-overflow undefined behavior
#if defined(__has_feature)
#if __has_feature(__address_sanitizer__)
    __attribute__((__no_sanitize__("float-cast-overflow")))
#endif
#endif
{
  // Don't allow a fraction of nodes less than 0. Usually this is a
  // configuration error.
  if (f <= 0.0) {
    f = 1.0;
  }
  fractionOfNodes_ = f;
  fractionCutoff_ = static_cast<size_t>(f * numeric_limits<size_t>::max());
}

bool JobFilters::isNonTriviallyEmpty() const {
  // We might have a configuration that is actually 'empty' (always matches)
  // even if some filters are set (if they are set to trivial values).
  return
    !cb_
    && whitelist_.empty()
    && whitelistRegex_.empty()
    && blacklist_.empty()
    && blacklistRegex_.empty()
    && tagWhitelist_.empty()
    && fractionOfNodes_ == 1.0;
}

}}
