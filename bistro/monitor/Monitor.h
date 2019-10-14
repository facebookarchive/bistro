/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Conv.h>
#include <folly/Synchronized.h>
#include <folly/experimental/ThreadedRepeatingFunctionRunner.h>
#include <memory>

#include "bistro/bistro/statuses/TaskStatus.h"

namespace facebook { namespace bistro {

class ConfigLoader;
class MonitorError;
class NodesLoader;
class TaskStatuses;
class Job;
typedef std::shared_ptr<const Job> JobPtr;

/**
 * Asynchronously computes information useful for monitoring.
 */
class Monitor final {
public:
  Monitor(
    std::shared_ptr<ConfigLoader> config_loader,
    std::shared_ptr<NodesLoader> nodes_loader,
    std::shared_ptr<TaskStatuses> task_statuses
  );
  ~Monitor();

  typedef std::pair<int, std::vector<std::string>> CountWithSamples;
  typedef std::map<TaskStatusBits, CountWithSamples> HistogramRow;
  typedef std::unordered_map<std::string, HistogramRow> LevelHistograms;
  typedef std::unordered_map<int, LevelHistograms> JobHistograms;

  JobHistograms getHistograms(const std::vector<const Job*>& jobs);

  time_t getLastUpdateTime() const {
    return lastUpdateTime_.load();
  }

  /**
   * Report scheduler errors to be displayed in the UI.
   */
  const std::unordered_map<std::string, std::string> copyErrors() const {
    return errors_.copy();
  }

protected:
  friend class MonitorError;

  void setErrors(
      const std::string& key,
      const std::vector<std::string>& messages) {

    SYNCHRONIZED(errors_) {
      if (messages.empty()) {
        errors_.erase(key);
      } else {
        errors_[key] = folly::join(".\n", messages);
      }
    }
  }

private:

  std::chrono::milliseconds update() noexcept;

  std::shared_ptr<ConfigLoader> configLoader_;
  std::shared_ptr<NodesLoader> nodesLoader_;
  std::shared_ptr<TaskStatuses> taskStatuses_;
  folly::Synchronized<std::unordered_map<std::string, std::string>> errors_;

  folly::Synchronized<JobHistograms> histograms_;

  std::atomic<time_t> lastUpdateTime_;

  // CAUTION: Declared last since the threads access other members of `this`.
  folly::ThreadedRepeatingFunctionRunner backgroundThreads_;
};

/**
 * Each error type should be cleared by code that might generate it. The
 * solution is to instantiate a MonitorError at the start of any
 * computation.  If, by destruction time, an error has not been reported, it
 * will be cleared from the Monitor. Use this helpful preprocessor macro
 * to capture the file & line number:
 *
 *   DECLARE_MONITOR_ERROR(monitor, error, "your key");
 *   ...
 *   error.report("your message");
 *   error.report("another message");
 */
class MonitorError {
public:
  MonitorError(Monitor* monitor, const std::string& key)
    : monitor_(monitor), key_(key) {}
  MonitorError(std::shared_ptr<Monitor> monitor, const std::string& key)
    : monitor_(monitor.get()), key_(key) {}
  ~MonitorError() {
    if (monitor_) {
      monitor_->setErrors(key_, messages_);
    }
  }

  template<typename... Args>
  std::string report(Args&&... args) {
    auto message =
      folly::to<std::string>(key_, ": ", std::forward<Args>(args)...);
    if (monitor_) {
      messages_.push_back(message);
    }
    return message;
  }

private:
  // Stores a dumb pointer because this class is assumed to exist
  // transiently, while the Monitor has a long lifetime.
  Monitor* monitor_;
  const std::string key_;
  std::vector<std::string> messages_;
};

#define DEFINE_MONITOR_ERROR(monitor, var_name, key) \
  ::facebook::bistro::MonitorError var_name( \
    (monitor), \
    ::folly::to<std::string>((key), " (", __FILE__, ":", __LINE__, ")") \
  )

}}  // namespace facebook::bistro
