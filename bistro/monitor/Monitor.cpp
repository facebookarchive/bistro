/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/monitor/Monitor.h"

#include <folly/experimental/AutoTimer.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/ConfigLoader.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/nodes/Nodes.h"
#include "bistro/bistro/nodes/NodesLoader.h"
#include "bistro/bistro/statuses/TaskStatuses.h"
#include "bistro/bistro/flags/Flags.h"

DEFINE_int32(monitor_samples, 10, "Max number of samples monitor returns");
DEFINE_int32(monitor_update_ms, 5000, "How often to update job histograms");

namespace facebook { namespace bistro {

using namespace std;
using namespace folly;

Monitor::Monitor(
    shared_ptr<ConfigLoader> config_loader,
    shared_ptr<NodesLoader> nodes_loader,
    shared_ptr<TaskStatuses> task_statuses)
  : configLoader_(config_loader),
    nodesLoader_(nodes_loader),
    taskStatuses_(task_statuses) {

  // CAUTION: ThreadedRepeatingFunctionRunner recommends two-stage
  // initialization for starting threads.  This specific case is safe since:
  //  - this comes last in the constructor, so the class is fully constructed,
  //  - this class is final, so no derived classes remain to be constructed.
  backgroundThreads_.add("Monitor", bind(&Monitor::update, this));
}

Monitor::~Monitor() {
  backgroundThreads_.stop();
}

Monitor::JobHistograms Monitor::getHistograms(
    const std::vector<const Job*>& jobs) {
  Monitor::JobHistograms ret;
  SYNCHRONIZED_CONST(histograms_) {
    for (const auto job : jobs) {
      auto it = histograms_.find(job->id());
      if (it != histograms_.end()) {
        ret[it->first] = it->second;
      }
    }
  }
  return ret;
}

std::chrono::milliseconds Monitor::update() noexcept {
  DEFINE_MONITOR_ERROR(this, error, "Updating monitor histogram");
  try {
    folly::AutoTimer<> timer;
    const auto now = time(nullptr);  // for isInBackoff
    JobHistograms hists;
    auto config = configLoader_->getDataOrThrow();
    auto status_snapshot = taskStatuses_->copySnapshot();
    auto nodes = nodesLoader_->getDataOrThrow();
    for (const auto& job_pair : config->jobs) {
      const auto& job = job_pair.second;
      if (!job->isValid()) {
        continue;
      }
      if (!status_snapshot.isJobLoaded(job->id())) {
        LOG(INFO) << error.report(
          "Monitor transiently not making a histogram for ", job->name(),
          " since it is not loaded"
        );
        continue;
      }

      auto& hist_level = hists[job->id()];

      auto row = status_snapshot.getRow(job->id());
      unordered_map<Node::ID, std::pair<TaskStatusBits, const Node*>> node_bits;
      for (const auto& node : *nodes) {
        if (node->level() != job->levelForTasks()) {
          continue;
        }
        TaskStatusBits b = TaskStatusBits::Empty;
        TaskStatusBits b2 = TaskStatusBits::Empty;

        // First we check if it can even run
        switch (job->shouldRunOn(*node)) {
          case Job::ShouldRun::Yes:
            // If we can run on this node, default to 'unstarted' rather than
            // empty.
            b2 = TaskStatusBits::Unstarted;
            break;
          case Job::ShouldRun::NoAvoided:
            b = TaskStatusBits::Avoided;
            break;
          case Job::ShouldRun::NoDisabled:
            b = TaskStatusBits::Disabled;
            break;
        }

        // Now apply the status we know about
        if (auto* status = row.getPtr(node->id())) {
          b2 = status->bits();
          // The monitor interprets UsesBackoff as "is in backoff right now".
          if (!status->isInBackoff(now)) {
            b2 = b2 & ~TaskStatusBits::UsesBackoff;
          }
        }
        // Now apply bits to this node and every parent
        for (const auto& n : node->traverseUp()) {
          node_bits[n.id()] = make_pair(
            node_bits[n.id()].first | b | b2,
            &n
          );
        }
      }

      for (const auto& pair : node_bits) {
        const auto bits = pair.second.first;
        const auto* node = pair.second.second;
        const string& level = config->levels.lookup(node->level());
        auto& h = hist_level[level];
        ++h[bits].first;
        if (h[bits].second.size() < FLAGS_monitor_samples) {
          h[bits].second.push_back(node->name());
        }
      }
    }
    histograms_.swap(hists);
    lastUpdateTime_.store(time(nullptr));
    if (FLAGS_log_performance) {
      timer.log("Monitor update finished");
    }
  } catch (const exception& e) {
    LOG(ERROR) << error.report(e.what());
    histograms_->clear();
  }
  return std::chrono::milliseconds(FLAGS_monitor_update_ms);
}

}}  // namespace facebook::bistro
