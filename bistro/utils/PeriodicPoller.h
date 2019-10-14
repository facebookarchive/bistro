/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <condition_variable>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <glog/logging.h>
#include <memory>
#include <string>
#include <thread>

#include "folly/experimental/ThreadedRepeatingFunctionRunner.h"

namespace facebook { namespace bistro {

/**
 * If you want to use a background thread to periodically poll for updates
 * to a certain piece of data, while handling transient exceptions, the
 * correct logic can be tricky to write.  This utility assures a correct
 * implementation.
 *
 * This class helps in several ways:
 *
 *  - It curries results / exceptions (akin to std::future).
 *
 *  - It polls on construction, so you can immediately get a valid value.
 *
 *  - It is hard to correctly use threads that are bound to class lifetimes
 *    (see the docblock of ThreadedRepeatingFunctionRunner).  This class
 *    makes it safe by handling state & thread lifetimes correctly, and
 *    providing just one thread-safe accessor: getDataOrThrow().  You can
 *    still break it by providing a fetch_raw_data that captures 'this', or
 *    members of your class that aren't fully initialized, but it's harder
 *    to accidentally shoot yourself in the foot.
 *
 *  - Most pollers fetch from an external data source (which may fail
 *    transiently), and then perform side-effect-free parsing (which will
 *    always fail until the data changes).  You may define processRawData()
 *    to avoid pointlessly hitting the same exception due to transient fetch
 *    failures.
 *
 *  - If the data you are polling has a version or modification time,
 *    fetchRawData can return 'false' to signal that the data has not
 *    changed, short-circuiting further processing.
 *
 *  - In the presence of exceptions, the version tracking is easy to get
 *    wrong.  PollerState protects you from the common errors.  It must be
 *    default-constructible, and support assignment.  It will only be used
 *    in the background thread.  The PollerState is always re-created on
 *    exception, protecting you from subtle bugs -- e.g. memoizing bad
 *    objects (like sockets), or:
 *      - fetch good data
 *      - hit transient exception
 *      - fetch the same good data
 *      - erroneously return nullptr since you cached only the good state
 *
 *   - There are two polling intervals: normal (period) and after an
 *     exception (retry_period).
 *
 * DO: This is already exercised by e.g. FileConfigPoller's test, but a
 * dedicated test would be better.
 */
template<
  class RawData,
  class PollerState,
  class ProcessedData,
  // This must be a pure / side-effect-free method. It must not throw
  // "transient" exceptions, because if it throws once, and the fetched data
  // does not change, then it will not be called again.  If your processing
  // does not satisfy this criterion, put it in fetchRawData, and make this
  // the identity function.
  std::shared_ptr<const ProcessedData> processRawData(
    // nullptr if the previous refresh had an exception, or in the 1st refresh
    std::shared_ptr<const ProcessedData> previous,
    const RawData&
  )
>
class PeriodicPoller final : boost::noncopyable {

/**
 * Fetch the data being polled. If it has changed, assign it to
 * out_raw_data, and return true.  Otherwise, return false, and out_raw_data
 * will not be used.
 *
 * Use the state to track if the raw data had changed -- it might store a
 * 'last modified time' or a version.  If the previous refresh succeeded,
 * you will get the state from the previous run.  Otherwise, you'll get a
 * default-initialized PollerState, which should force you to treat any
 * data you fetched as new (any other behavior is most likely an error).
 *
 * Not a template argument to allow the use of bound methods & lambdas.
 *
 * DANGER: the calling class should not bind 'this' to any function it
 * might want to pass in here, since the containing instance is only
 * partially initialized.  If you must, capture specific member
 * variables that had already been properly initialized.
 */
typedef std::function<bool (
  RawData* out_raw_data,
  // default-initialized unless the previous refresh succeeded
  PollerState* state,
  // nullptr if the previous refresh had an exception, or in the 1st refresh
  std::shared_ptr<const ProcessedData> previous
)> FetchRawDataFn;

public:
  PeriodicPoller(
    std::string thread_name,  // up to 15 chars
    FetchRawDataFn fetch_raw_data,  // see the typedef
    std::chrono::milliseconds period,
    std::chrono::milliseconds retry_period
  ) : fetchRawData_(std::move(fetch_raw_data)),
      period_(period),
      retryPeriod_(retry_period),
      // Call the default constructor explicitly, so that we value initialize
      // any basic types (like int). Otherwise they will have some garbage
      // values.
      loaderState_() {

    // Make the first call to getDataOrThrow return real data.
    auto initial_period = refresh();
    // Though the ThreadedRepeatingFunctionRunner docs urge
    // double-initialization, it is safe to make this thread in the
    // constructor, since:
    //  - this is the last action in the constructor
    //  - this class is guaranteed not to have derived classes
    //  - the threads_ object is declared last, and hence is destroyed first
    threads_.add(
      std::move(thread_name),
      std::bind(&PeriodicPoller::refresh, this),
      initial_period
    );
  }

  ~PeriodicPoller() {
    threads_.stop();
  }

  /**
   * Returns the latest Data, or throws if there was an error fetching it.
   * Only copies the shared_ptr, since the Data object is immutable.
   * Thread-safe.
   *
   * The initial state of the internal ProcessedData pointer is nullptr.
   * However, you will never get nullptr from getDataOrThrow, unless your
   * code does one of these:
   *  - Your processRawData() returns nullptr.
   *  - Your fetchRawData() returns false on the initial refresh(), when the
   *    PollerState was default-initialized.  That's not the right behavior
   *    for most uses.
   */
  std::shared_ptr<const ProcessedData> getDataOrThrow() const {
    auto r = result_.copy();
    if (r.ex_.hasValue()) {
      throw std::runtime_error(r.ex_.value());
    }
    return r.data_;
  }

private:
  // Similarly to std::future, holds either a data pointer, or an exception.
  struct Result {
    // This is null initially, and after exceptions, until the first
    // successful refresh()
    std::shared_ptr<const ProcessedData> data_;
    // The callers of getDataOrThrow don't care to distinguish exception
    // types, but if it did, folly::exception_wrapper would help.
    folly::Optional<std::string> ex_;

    void setData(std::shared_ptr<const ProcessedData>&& data) noexcept {
      data_ = std::move(data);
      ex_ = folly::none;
    }

    void setException(const std::exception& e) noexcept {
      data_.reset();
      ex_ = e.what();
    }
  };

  // Invokes fetchRawData, processRawData and stores the result. Returns the
  // appropriate delay depending on whether an exception was thrown.
  std::chrono::milliseconds refresh() noexcept {
    auto previous = result_->data_;
    RawData raw_data;
    try {
      if (!fetchRawData_(&raw_data, &loaderState_, previous)) {
        return result_->ex_ ? retryPeriod_ : period_;
      }
    } catch (const std::exception& e) {
      // On exception, reset the fetcher state, see docblock for the reasons.
      loaderState_ = PollerState();
      result_->setException(e);
      return retryPeriod_;
    }
    try {
      // Don't hold the result_ lock while processing
      auto new_data = processRawData(previous, raw_data);
      result_->setData(std::move(new_data));
      return period_;
    } catch (const std::exception& e) {
      result_->setException(e);
      return retryPeriod_;
    }
  }

  const FetchRawDataFn fetchRawData_;
  const std::chrono::milliseconds period_;
  const std::chrono::milliseconds retryPeriod_;

  // Only accessed from refresh(), so no synchronization
  PollerState loaderState_;
  // Uninitialized until the first call to getDataOrThrow
  folly::Synchronized<Result> result_;

  // Declared last since the thread's callback may access other members.
  folly::ThreadedRepeatingFunctionRunner threads_;
};

}}
