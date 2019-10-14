/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/experimental/AutoTimer.h>
#include <folly/io/async/EventBase.h>
#include <folly/MPMCQueue.h>

#include "bistro/bistro/processes/AsyncSubprocess.h"

using namespace facebook::bistro;

void checkSleepInterrupted(bool wait_to_signal) {
  // Could've used a raw pointer, but TestWaitpidVsSignalRace tests it already
  auto queue = std::make_shared<folly::MPMCQueue<int>>(1);
  folly::EventBase evb;
  auto ret_code_future = asyncSubprocess(
    &evb,
    folly::Subprocess{std::vector<std::string>{"/bin/sleep", "86400"}},
    sendSignalCallback(queue)
  );

  if (wait_to_signal) {
    // Drive the EventBase for ~50ms to let things have a chance to go wrong.
    for (size_t i = 0; i < 5; ++i) {
      /* sleep override */
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      evb.loopOnce(EVLOOP_NONBLOCK);
    }
  }
  ASSERT_FALSE(ret_code_future.isReady());

  queue->blockingWrite(SIGTERM);
  while (!ret_code_future.isReady()) { evb.loop(); }

  auto ret_code = std::move(ret_code_future).get();
  EXPECT_TRUE(ret_code.killed());
  EXPECT_EQ(SIGTERM, ret_code.killSignal());
}

TEST(TestAsyncSubprocess, SleepInterrupted) {
  checkSleepInterrupted(/*wait_to_signal=*/ false);
  checkSleepInterrupted(/*wait_to_signal=*/ true);
}

TEST(TestAsyncSubprocess, TestWaitpidVsSignalRace) {
  constexpr size_t kNumThreads = 100;
  constexpr size_t kSleepMs = 1000;

  // Messages from threads, preceded by the thread ID.
  folly::MPMCQueue<std::pair<size_t, folly::ProcessReturnCode>>
    ret_code_queue(kNumThreads);

  // Create the signal flags first, so that they live longer than the
  // threads and have stable addresses.
  std::vector<OneSignalFlag> sigterms;
  for (size_t i = 0; i < kNumThreads; ++i) {
    sigterms.emplace_back(OneSignalFlag{SIGTERM});
  }
  // Spin up a bunch of threads, with a short-running process in each one.
  std::vector<std::thread> threads;
  SCOPE_EXIT {
    for (auto&& thread : threads) { thread.join(); }
  };

  folly::AutoTimer<> timer;
  using Clock = std::chrono::high_resolution_clock;
  auto start = Clock::now();
  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, i]() {
      folly::EventBase evb;
      auto ret_code_future = asyncSubprocess(
        &evb,
        folly::Subprocess{std::vector<std::string>{
          "/bin/sleep", folly::to<std::string>(0.001 * kSleepMs)
        }},
        sendSignalCallback(&sigterms[i])
      );
      while (!ret_code_future.isReady()) { evb.loop(); }
      ret_code_queue.blockingWrite(
          std::make_pair(i, std::move(ret_code_future).get()));
    });
  }
  auto startup_duration = Clock::now() - start;
  auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    startup_duration
  ).count();
  if (startup_ms > 50) {
    LOG(ERROR) << "It looks like this system is heavily loaded, so the "
      << "race test is unlikely to be reliable. Not checking outcome.";
    return;
  }
  timer.log("Started ", kNumThreads, " threads of 'sleep ", kSleepMs, "'");

  // This wait is an attempt to start killing just as the first sleep exits.
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(
    kSleepMs - startup_ms
  ));
  timer.log("Waited for 'sleep' processes to start exiting");

  // Spread out the signaling over time to maximize the odds of racing.
  auto inter_signal_wait = startup_duration / (10.0 * kNumThreads);
  for (size_t i = 0; i < kNumThreads; ++i) {
    sigterms[i].sendSignal();
    /* sleep override */
    std::this_thread::sleep_for(inter_signal_wait);
  }
  timer.log(
    "Finished sending SIGTERM to the 'sleep' processes with an interval of ",
    std::chrono::duration_cast<std::chrono::microseconds>(inter_signal_wait)
    .count(), " usec"
  );

  size_t sum_thread_ids_2 = 0;
  size_t num_killed = 0;
  size_t num_exited = 0;
  for (size_t i = 0; i < kNumThreads; ++i) {
    std::pair<size_t, folly::ProcessReturnCode> p;
    ret_code_queue.blockingRead(p);
    sum_thread_ids_2 += p.first;
    if (p.second.killed()) {
      ++num_killed;
      EXPECT_EQ(SIGTERM, p.second.killSignal());
      int _;
      EXPECT_FALSE(sigterms[p.first].read(_));  // Signal was sent
    } else {
      ++num_exited;
      ASSERT_TRUE(p.second.exited());
      EXPECT_EQ(0, p.second.exitStatus());
      // We cannot be completely sure that the signal was not sent.  It
      // could have been sent just after the process exited, but before we
      // waited for it.  I cannot be bothered to make a robust test out of
      // this observation, so leaving it out:
      // EXPECT_TRUE(sigterms[p.first].popSignal());
    }
  }
  timer.log("Exited: ", num_exited, ", killed: ", num_killed);
  EXPECT_EQ(kNumThreads * (kNumThreads - 1) / 2, sum_thread_ids_2);

  // Future: These test conditions are crappy/flaky, and require some
  // system-specific tuning (see the "/ 10.0" above).  Can I do any better?
  // If not, consider deleting these, and just leaving this test as a way of
  // exercising many concurrent processes.
  //
  // Trusting the law of large numbers (and assuming the OS is uniform-ish).
  EXPECT_LT(0, num_exited);
  EXPECT_LT(0, num_killed);

  // threads are joined via SCOPE_EXIT
}
