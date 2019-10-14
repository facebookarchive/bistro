/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/date_time/local_time/local_time_types.hpp>

#include "bistro/bistro/cron/CrontabItem.h"
#include "bistro/bistro/cron/CrontabSelector.h"
#include "bistro/bistro/cron/utils/date_time.h"

// Add stateful Cron support for more robustness, see README for a design.

namespace folly {
  class dynamic;
}

namespace boost { namespace posix_time {
  class ptime;
}}

namespace facebook { namespace bistro { namespace detail_cron {

class MatchComputeState;  // Declared in CPP file

class StandardCrontabItem : public CrontabItem {
public:
  // The positions we use to search through local time labels.  We use
  // human-readable values here, so some are 0-based and some are 1-based.
  // This is nice since it matches the boost::date_time constructors.
  enum class Position {
    YEAR,  // 1970..?
    MONTH,  // 1..12
    DAY_OF_MONTH,  // 1..28-31; day of week is computed
    HOUR,  // 0..23
    MINUTE,  // 0..59
    MAX
  };

  StandardCrontabItem(
    const folly::dynamic&, boost::local_time::time_zone_ptr
  );

  /**
   * Even though we currently only support 1-minute resolution, you must not
   * assume that the output will be aligned on the minute (it usually will
   * be, except for "DST skip" cases).
   *
   * After finding the first local time label that matches this item's
   * selectors, it may turn out that it maps to 0, 1, or 2 UTC timestamps as
   * a consequence of DST clock forwards or rewinds (for details, see
   * timezoneLocalPTimeToUTCTimestamps() docs).  We use the item's
   * configured heuristic to deal with this (see comments about the field
   * "dst_fixes" below).
   */
  folly::Optional<time_t> findFirstMatch(time_t time_since_utc_epoch)
    const final;
  bool isTimezoneDependent() override { return true; }

  std::string getPrintable() const override; // For testing only, see base class

private:

  // Here are the allowed values that can occur in the "dst_fixes" list,
  // grouped by policy.  You must specify exactly one entry for each policy,
  //
  // Forward (DST sets the clock ahead, skipping a range of local times):
  //  "unskip": [good for rare events]
  //      All matches in the skipped range are mapped to a single match in
  //      the instant before the skip.
  //  "skip": [good for frequent events]
  //      Cron matches in the skipped range of local times are ignored.
  //
  // Rewind (DST sets the clock back, making some local times ambiguous):
  //  "repeat_use_only_early": [good for rare events]
  //      For local time matches that are ambiguous, match only the times
  //      before the clock is rewound.
  //  "repeat_use_only_late": [good for rare events]
  //      For local time matches that are ambiguous, match only the times
  //      after the clock is rewound.
  //  "repeat_use_both":  [good for frequent events]
  //      For local time matches that are ambiguous, match both times
  //      (before and after the clock is rewound).
  const static short kDSTPolicyForward = 0x1;
  const static short kDSTPolicyRewind = 0x2;
  const static short kRequiredDSTPolicies = 0x3;

  // These correspond to the actual actions the code has to take.
  const static short kDSTForwardDoUnskip = 0x1;  // "skip" sets no bits
  // "repeat_use_both" sets both of the following bits
  const static short kDSTRewindUseEarly = 0x2;
  const static short kDSTRewindUseLate = 0x4;

  void parseDstFixes(const folly::dynamic&);

  folly::Optional<time_t> findFirstMatchImpl(time_t time_since_utc_epoch)
    const;
  folly::Optional<UTCTimestampsForLocalTime> findUTCTimestampsForFirstMatch(
    time_t, boost::posix_time::ptime* match_pt
  ) const;
  void findMatchingPositions(MatchComputeState *s, int64_t carry_steps)
    const;
  void findMatchingPositionsImpl(MatchComputeState *s, int64_t carry_steps)
    const;
  void findMatchingDayOfWeek(MatchComputeState *s, int64_t carry_steps) const;

  int dstFixes_;

  CrontabSelector selectors_[(int)Position::MAX];
  folly::Optional<CrontabSelector> maybeDayOfWeekSel_;
};

}}}
