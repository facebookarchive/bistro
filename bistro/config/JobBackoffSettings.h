#pragma once

#include <unordered_map>

#include "folly/dynamic.h"

#include "bistro/bistro/if/gen-cpp2/common_types.h"

namespace facebook { namespace bistro {

class JobBackoffSettings {

public:
  JobBackoffSettings();
  explicit JobBackoffSettings(const folly::dynamic& d);

  cpp2::BackoffDuration getNext(const cpp2::BackoffDuration& cur) const;

  folly::dynamic toDynamic() const;

  bool operator==(const JobBackoffSettings&) const;
  bool operator!=(const JobBackoffSettings&) const;

private:
  std::vector<int> values_;
  bool repeat_;

};

}}
