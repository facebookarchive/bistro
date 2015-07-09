/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace facebook { namespace bistro {

/**
 * An object storing one or more 'background' threads to do some work.
 * Threads and classes don't mix well in C++, so you have to be very
 * careful.  A reasonable pattern looks like this:
 *
 * struct MyClass {
 *   // Note that threads are NOT added in the constructor, for two reasons:
 *   //
 *   //   (1) If you added some, and had any subsequent initialization (e.g.
 *   //       derived class constructors), 'this' would not be fully
 *   //       constructed when the worker threads came up, causing
 *   //       heisenbugs.
 *   //
 *   //   (2) Also, if your constructor threw after thread creation, the
 *   //       class destructor would not be invoked, potentially leaving the
 *   //       threads running too long.
 *   //
 *   // It's better to have explicit two-step initialization, or to lazily
 *   // add threads the first time they are needed.
 *   MyClass() : count_(0) {}
 *
 *   // You must stop the threads as early as possible in the destruction
 *   // process (or even before).  In the case of a class hierarchy, the
 *   // final class MUST always call stop() as the first thing in its
 *   // destructor -- otherwise, the worker threads may access already-
 *   // destroyed state.
 *   ~MyClass() {
 *     // if MyClass is abstract:
 *     threads_.stopAndWarn("MyClass");
 *     // Otherwise:
 *     threads_.stop();
 *   }
 *
 *   // See the constructor for why two-stage initialization is preferred.
 *   void init() {
 *     threads_.add(bind(&MyClass::incrementCount, this));
 *   }
 *
 *   std::chrono::milliseconds incrementCount() {
 *     ++count_;
 *     return 10;
 *   }
 *
 * private:
 *   std::atomic<int> count_;
 *   // It's safest for this member to be last in the class, so that all
 *   // threads are guaranteed to be stopped before any of the members they
 *   // are accessing might be destroyed.
 *   BackgroundThreads threads_;
 * }
 */
class BackgroundThreads {
public:
  BackgroundThreads();
  virtual ~BackgroundThreads();

  /**
   * Ideally, you will call this before initiating the destruction of the
   * host object.  Otherwise, this should be the first thing in the
   * destruction sequence.  If it comes any later, worker threads may access
   * class state that had already been destroyed.
   */
  void stop();

  /**
   * Must be called at the TOP of the destructor of any abstract class that
   * contains BackgroundThreads (directly or through a parent).  Any
   * non-abstract class destructor must instead stop() at the top.
   */
  void stopAndWarn(const std::string& class_of_destructor);

  /**
   * Do NOT use this in a constructor. Not thread-safe.
   *
   * Run a function in a background loop, sleeping the returned time between
   * calls. Optionally sleep a given amount of time before the first call.
   */
  void add(
    std::function<std::chrono::milliseconds()> f,
    std::chrono::milliseconds initialSleepAmount = std::chrono::milliseconds(0)
  );

  size_t size() const { return threads_.size(); }

private:
  void executeInLoop(
    std::function<std::chrono::milliseconds()> f,
    std::chrono::milliseconds initialSleepAmount
  ) const;

  /**
   * Sleep in increments of 1 second. This allows us to detect when the program
   * is exiting without having to wait until the full sleep duration has
   * elapsed.
   */
  void incrementalSleep(std::chrono::milliseconds amount) const;

  std::atomic<bool> keepRunning_;
  std::vector<std::thread> threads_;
};

}}
