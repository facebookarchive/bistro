/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/remote/WorkerSetID.h"

using namespace facebook::bistro;

cpp2::BistroInstanceID instanceID(int64_t start_time, int64_t rand) {
  cpp2::BistroInstanceID id;
  id.startTime = start_time;
  id.rand = rand;
  return id;
}

// Hide from the "integer overflow in expression" warning.
int64_t add(int64_t a, int64_t b) {
  return static_cast<int64_t>(
      static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

TEST(TestWorkerSetHash, Exact) {
  // Add 2^62 to make sure 'add' overflows.
  const int64_t s1 = (1LLU << 62) + 938249;
  const int64_t r1 = (1LLU << 62) + 123455;
  const int64_t s2 = (1LLU << 62) + 4559023894911;
  const int64_t r2 = (1LLU << 62) + 7231233244239;

  const auto id1 = instanceID(s1, r1);
  const auto id2 = instanceID(s2, r2);

  cpp2::WorkerSetHash w1w2;
  addWorkerIDToHash(&w1w2, id1);
  addWorkerIDToHash(&w1w2, id2);
  EXPECT_EQ(2, w1w2.numWorkers);

  cpp2::WorkerSetHash w2w1;
  addWorkerIDToHash(&w2w1, id2);
  addWorkerIDToHash(&w2w1, id1);

  EXPECT_EQ(w1w2, w2w1);  // Commutative

  // Also check exact values
  EXPECT_EQ(add(s1, s2), w1w2.startTime.addAll);
  EXPECT_EQ(add(r1, r2), w1w2.rand.addAll);
  EXPECT_EQ(s1 ^ s2, w1w2.startTime.xorAll);
  EXPECT_EQ(r1 ^ r2, w1w2.rand.xorAll);

  // Removing w2 should leave w1
  removeWorkerIDFromHash(&w1w2, id2);
  cpp2::WorkerSetHash w1;
  addWorkerIDToHash(&w1, id1);
  EXPECT_EQ(w1, w1w2);

  // Removing w1 should leave w2, also check the exact value.
  removeWorkerIDFromHash(&w2w1, id1);
  cpp2::WorkerSetHash w2;
  addWorkerIDToHash(&w2, id2);
  EXPECT_EQ(w2, w2w1);
  EXPECT_EQ(id2.startTime, w2w1.startTime.addAll);
  EXPECT_EQ(id2.startTime, w2w1.startTime.xorAll);
  EXPECT_EQ(id2.rand, w2w1.rand.addAll);
  EXPECT_EQ(id2.rand, w2w1.rand.xorAll);
  EXPECT_EQ(1, w2w1.numWorkers);

  // Removing both leaves the empty hash.
  removeWorkerIDFromHash(&w2w1, id2);
  removeWorkerIDFromHash(&w1w2, id1);
  EXPECT_EQ(cpp2::WorkerSetHash(), w2w1);
  EXPECT_EQ(cpp2::WorkerSetHash(), w1w2);
}

#define EXPECT_EARLIER(a, b) { \
  auto _EXPECT_EARLIER_a = (a); \
  auto _EXPECT_EARLIER_b = (b); \
  EXPECT_TRUE(WorkerSetIDEarlierThan()(_EXPECT_EARLIER_a, _EXPECT_EARLIER_b)) \
    << _EXPECT_EARLIER_a << " not earlier than " << _EXPECT_EARLIER_b; \
}

#define EXPECT_NOT_EARLIER(a, b) { \
  auto _EXPECT_EARLIER_a = (a); \
  auto _EXPECT_EARLIER_b = (b); \
  EXPECT_FALSE(WorkerSetIDEarlierThan()(_EXPECT_EARLIER_a, _EXPECT_EARLIER_b))\
    << _EXPECT_EARLIER_a << " not earlier than " << _EXPECT_EARLIER_b; \
}

// Separate test with a *DeathTest name as per GTest best practices.
TEST(WorkerSetVersionDeathTest, EarlierThan) {
  ASSERT_DEATH({
    WorkerSetIDEarlierThan()(0x8000000000000000, 0);
  }, "Versions differ by 2\\^63:");
  ASSERT_DEATH({
    WorkerSetIDEarlierThan()(0x7fffffffffffffff, 0xffffffffffffffff);
  }, "Versions differ by 2\\^63:");
}

TEST(TestWorkerSetHash, EarlierThan) {
  EXPECT_NOT_EARLIER(0, 0);
  EXPECT_NOT_EARLIER(1, 0);
  EXPECT_NOT_EARLIER(0x7ffffffffffffffe, 0);
  EXPECT_NOT_EARLIER(0x7fffffffffffffff, 0);
  EXPECT_EARLIER(0x8000000000000001, 0);
  EXPECT_EARLIER(0x8000000000000002, 0);
  EXPECT_EARLIER(0xfffffffffffffffe, 0);
  EXPECT_EARLIER(0xffffffffffffffff, 0);

  EXPECT_NOT_EARLIER(1, 1);
  EXPECT_NOT_EARLIER(2, 1);
  EXPECT_NOT_EARLIER(0x7ffffffffffffffe, 1);
  EXPECT_NOT_EARLIER(0x7fffffffffffffff, 1);
  EXPECT_NOT_EARLIER(0x8000000000000000, 1);
  EXPECT_EARLIER(0x8000000000000002, 1);
  EXPECT_EARLIER(0x8000000000000003, 1);
  EXPECT_EARLIER(0xfffffffffffffffe, 1);
  EXPECT_EARLIER(0xffffffffffffffff, 1);
  EXPECT_EARLIER(0, 1);

  EXPECT_NOT_EARLIER(0x7ffffffffffffffe, 0x7ffffffffffffffe);
  EXPECT_NOT_EARLIER(0x7fffffffffffffff, 0x7ffffffffffffffe);
  EXPECT_NOT_EARLIER(0x8000000000000000, 0x7ffffffffffffffe);
  EXPECT_NOT_EARLIER(0xfffffffffffffffc, 0x7ffffffffffffffe);
  EXPECT_NOT_EARLIER(0xfffffffffffffffd, 0x7ffffffffffffffe);
  EXPECT_EARLIER(0xffffffffffffffff, 0x7ffffffffffffffe);
  EXPECT_EARLIER(0, 0x7ffffffffffffffe);
  EXPECT_EARLIER(1, 0x7ffffffffffffffe);
  EXPECT_EARLIER(0x7ffffffffffffffc, 0x7ffffffffffffffe);
  EXPECT_EARLIER(0x7ffffffffffffffd, 0x7ffffffffffffffe);
}
