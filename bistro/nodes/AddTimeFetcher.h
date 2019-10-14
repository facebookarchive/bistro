/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/date_time/local_time/local_time_types.hpp>
#include <folly/json.h>
#include <unordered_map>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/cron/CrontabItem.h"
#include "bistro/bistro/nodes/NodeFetcher.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/utils.h"

namespace facebook { namespace bistro {

/**
 * This fetcher is very similar to Cron, with a child node generated for
 * each occurrence of a recurring event.
 *
 * It adds timestamps to the node names of the parent level, so each parent
 * node spawns a bunch of timestamped children.  These children are created
 * on a specific schedule, and they have limited lifetimes, so they come and
 * go periodically as time passes.
 *
 * Let's say you have 100 nodes, on which you want to run some tasks every
 * 15 minutes, and some other tasks every hour. Here is a config to do that:
 *
 *   "nodes": {
 *     "levels": {"something", "something_with_time"},
 *     "node_sources": [
 *       <<< some node source yielding 100 nodes with level "something" >>>,
 *       {
 *         "source": "add_time",
 *         "prefs": {
 *           "parent_level": "something",
 *           "schedule": [{
 *             "cron": {"epoch": {"period": 900}},
 *             "lifetime": 7200,
 *             "tags": ["15m"]
 *           }, {
 *             "cron": {"epoch": {"period": 3600}},
 *             "lifetime": 7200,
 *             "tags": ["60m"]
 *           }]
 *         }
 *       }
 *     ]
 *   }
 *
 * You will get 800 bottom-level nodes, 8 for each outer node.  Their names
 * will have ":<timestamp>" appended to them, with a period of 15 minutes.
 * All 8 nodes will have the tag "15m", and 2 of them will also have the tag
 * "60m".  You can then use tag filters to to run different jobs with
 * different periods.
 *
 * The "cron" field in "schedule" items is configured using the Cron library
 * JSON syntax, so refer to "Crontab items and selectors" in the library
 * README for more details on this.
 *
 * In particular, you can use standard Cron items in "schedule", e.g.
 *
 *   // Run at noon on the 10th and 20th of Jan, Mar, May, Jul, ..., Nov.
 *   {"cron": {
 *     "minute": 0,
 *     "hour": 12,
 *     "day_of_month": [10, 20],
 *     "month": {"period": 2}
 *   }}
 *
 * == Key details ==
 *
 *  * Nodes disappear "lifetime" seconds after being created.
 *
 *  * A node is disabled "enabled_lifetime" seconds after being created (by
 *    default, this is equal to "lifetime").
 *
 *    CAUTION: When several schedule items generate the same timestamp, the
 *    resulting node will be disabled if *any* of them disable it.  If this
 *    is undesired, offset your schedules by 1 second.
 *
 *  * The tags of a node are the *union* of the tags of all the schedule
 *    items that land on that node.
 *
 *  * "epoch" items generate nodes with timestamps start + period * [n >= 0]
 *    that are <= end.
 *
 * == Useful things to know ==
 *
 * === Bistro refreshes these nodes with a frequency of over a minute ===
 *
 * Bistro's node fetcher loop looks like this:
 *
 *   Forever:
 *     For each fetcher:
 *       Fetch
 *         Sleep
 *
 * This means that the AddTime fetcher will refresh as often as it takes us
 * to update all fetchers, plus the delay.  Currently, this means we update
 * with a frequency of ~60 seconds, assuming all the other node fetchers are
 * really fast.
 *
 * If you combine this with some really slow node fetcher, "add_time"
 * becomes useless for any kind of fine-grained scheduling.
 *
 * I don't think it's a problem now, but the user should beware.
 *
 * === Nodes follow standard Cron DST policy ===
 *
 * With standard Cron, if your specified event is during a DST transition
 * (e.g.  at 1:30AM), and your "dst_fixes" includes "repeat_use_both", then
 * you'll get a node both for the standard time and daylight savings time
 * 1:30AM for that day.  They have different UTC timestamps, so this node
 * fetcher is oblivious to the whole DST thing.
 *
 * Please see the Cron README to decide which "dst_fixes" to use.
 *
 * === We currently use the system time & timezone ===
 *
 * Since, the current code is stateless, we have no means of detecting
 * system time or timezone changes, and therefore no means of mitigating
 * them.  The consequences can vary, and it might even do what you'd expect
 * on your system, but it might not.  Current workaround: don't touch the
 * system time or timezone -- or temporarily turn off all jobs before
 * messing with time.
 *
 * Here are some potential ways to improve this:
 *
 * TODO: Refresh timezone data periodically, since TZ rules can change.  The
 * best way to do this in Bistro is probably a dedicated thread that calls
 * tzset() -- TODO: confirm that this tracks system timezone rule updates:
 *   http://brian.moonspot.net/2007/03/14/vixie-cron-and-the-new-us-dst/
 *
 * TODO: Add support for specifying a Boost "posix_time_zone" string as part
 * of this node fetcher.  Then, use that instead of the system timezone.
 * You still won't have immunity to time changes.  This is easy, but see the
 * README for an explanation of how the Boost strings are nonstandard.  This
 * could be confusing to users, so I'm not adding it.  I encourage you to
 * only add it if you make the POSIX => Boost translation function suggested
 * in the Cron README.
 *
 * TODO: (overengineering) Build stateful Cron (see the Cron README). Also
 * add support for stateful node fetchers to Bistro.  Use stateful Cron in
 * this fetcher, and let its time / time-zone change policies take care of
 * the issue.
 */

template<typename Clock>
class AddTimeFetcher : public NodeFetcher {
public:
 void fetch(const Config& config,
            const NodeConfig& node_config,
            Nodes* all_nodes) const override {
    using std::string;
    const auto& prefs = node_config.prefs;
    const auto& format_str =
      prefs.convert<string>("format", "{parent}:{time}");
    const auto time_to_tags_and_enabled = makeTimestampsToTagsAndEnabled(
      prefs.get("schedule", folly::dynamic::array()));

    // Use the "parent_level" pref to get my parents and my level ID.
    const auto my_level_and_parents =
      getMyLevelAndParents(config, node_config, all_nodes);
    std::map<string, string> format_args;
    for (const auto& parent : my_level_and_parents.second) {
      format_args["parent"] = parent->name();
      for (const auto& tags_and_enabled : time_to_tags_and_enabled) {
        format_args["time"] = folly::to<string>(tags_and_enabled.first);
        all_nodes->add(
          folly::vformat(format_str, format_args).str(),
          my_level_and_parents.first,
          tags_and_enabled.second.second,
          parent.get(),
          tags_and_enabled.second.first
        );
      }
    }
  }

private:

  typedef std::unordered_map<
    int64_t,
    std::pair<Node::TagSet, bool>
  > TimeToTagsAndEnabled;

  struct ScheduleItem {
    int64_t lifetime_;
    int64_t enabled_lifetime_;
    Node::TagSet tags_;
    std::unique_ptr<const CrontabItem> cron_;

    explicit ScheduleItem(const folly::dynamic& item_cfg)
      : lifetime_(0),  // Required; 0 is not a valid value
        enabled_lifetime_(-1) {  // Optional; defaults to "lifetime"
      if (!item_cfg.isObject()) {
        throw BistroException("\"schedule\" items must be objects");
      }
      for (const auto &p : item_cfg.items()) {
        if (p.first == "lifetime") {
          lifetime_ = p.second.asInt();
        } else if (p.first == "enabled_lifetime") {
          enabled_lifetime_ = p.second.asInt();
        } else if (p.first == "tags") {
          if (!p.second.isArray()) {
            throw BistroException("\"tags\" must be an array");
          }
          for (const auto &tag : p.second) {
            tags_.insert(tag.asString());
          }
        } else if (p.first == "cron") {
          boost::local_time::time_zone_ptr system_tz;  // null == system tz
          cron_ = CrontabItem::fromDynamic(p.second, system_tz);
        }
      }
      if (lifetime_ == 0) {
        throw BistroException("add_time: each item needs a lifetime");
      }
      if (enabled_lifetime_ == -1) {
        enabled_lifetime_ = lifetime_;
      }
    }
  };

  void addTimeTagsAndEnabled(
      TimeToTagsAndEnabled* tte,
      const ScheduleItem& item,
      int64_t cur_time,
      int64_t event_time) const {
    auto enabled = cur_time - event_time < item.enabled_lifetime_;
    auto i = tte->find(event_time);
    if (i != tte->end()) {
      i->second.first.insert(item.tags_.begin(), item.tags_.end());
      i->second.second = i->second.second && enabled;
    } else {
      tte->emplace(event_time, make_pair(item.tags_, enabled));
    }
  }

  // TODO: this code could be more performant if node fetchers were stateful.
  //  - So long as the schedule has not changed, we don't recompute any
  //    of our queues, of which there are 3:
  //      * the next upcoming timestamp for each schedule item
  //      * 2 separate enabled and disabled timestamp => tags maps
  //  - At each update, we move items between queues:
  //      * From upcoming to enabled as soon as the timestamp is in the past.
  //        We immediately place the next timestamp into the upcoming queue.
  //        (Caveat: if one is not found in the next hour or so, use
  //         a placeholder instead.)
  //      * From enabled to disabled upon expiration of "enabled_lifetime".
  //      * Delete from "disabled" upon expiration of "lifetime".
  //  - Nodes are generated on the basis of the enabled & disabled queues.
  TimeToTagsAndEnabled makeTimestampsToTagsAndEnabled(
    const folly::dynamic &items
  ) const {
    if (!items.isArray()) {
      throw BistroException("\"schedule\" must be an array");
    }
    TimeToTagsAndEnabled tte;
    int64_t cur_time = std::chrono::duration_cast<std::chrono::seconds>(
      Clock::now().time_since_epoch()
    ).count();
    for (const auto& item_cfg : items) {
      ScheduleItem item(item_cfg);
      int64_t event_time = cur_time - item.lifetime_ + 1;
      while (true) {
        auto maybe_time = item.cron_->findFirstMatch(event_time);
        if (!maybe_time.hasValue() || maybe_time.value() > cur_time) {
          break;
        }
        event_time = maybe_time.value();
        addTimeTagsAndEnabled(&tte, item, cur_time, event_time);
        ++event_time;
      }
    }
    return tte;
  }
};

}}
