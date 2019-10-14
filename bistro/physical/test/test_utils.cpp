/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/experimental/TestUtil.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "bistro/bistro/physical/test/utils.h"
#include "bistro/bistro/physical/utils.h"

DECLARE_string(nvidia_smi);

using namespace facebook::bistro;

struct TestPhysicalUtils : public ::testing::Test {
  template<typename T>
  void checkValue(T v) {
    EXPECT_TRUE(folly::writeFile(
      folly::to<std::string>(v, '\n'), tf.path().c_str()
    ));
    EXPECT_EQ(v, valueFromFile<T>(tf.path()));
  }

  template<typename T>
  void checkValueAndFailure(T v) {
    checkValue(v);
    EXPECT_TRUE(folly::writeFile(std::string("bad value"), tf.path().c_str()));
    EXPECT_THROW(valueFromFile<T>(tf.path()), std::runtime_error);
  }

  folly::test::TemporaryFile tf;
};

TEST_F(TestPhysicalUtils, ValueFromFile) {
  checkValueAndFailure(-1.23232);
  checkValueAndFailure((uint64_t)21314412124);
  checkValueAndFailure(-763248);
  checkValue(std::string("kitten train"));  // hard to test failure for strings
}

TEST_F(TestPhysicalUtils, ParseIntSet) {
  EXPECT_EQ(
    (std::set<uint64_t>{0, 1, 2, 4, 6, 7, 8}),
    parseIntSet("0-1,2,4,6-8")
  );
  EXPECT_EQ(17, parseIntSet("0-12,17,20,21-22").size());
  EXPECT_EQ(23, parseIntSet("0-12,7-9,8-22").size());

  EXPECT_EQ((std::set<uint64_t>{0, 1, 2}), parseIntSet("0-2"));
  EXPECT_THROW(parseIntSet("2-0"), std::runtime_error);

  EXPECT_THROW(parseIntSet(""), std::runtime_error);
  EXPECT_THROW(parseIntSet("0+12,17"), std::runtime_error);
  EXPECT_THROW(parseIntSet("0-12;17"), std::runtime_error);
}

TEST(TestNvidiaSmi, CheckArgs) {
  folly::test::ChangeToTempDir td;
  makeShellScript(
    "a",
    "test $# = 2 -a $1 = --format=csv,noheader,nounits "
      "-a $2 = --foo=bar,baz && echo borf"
  );
  FLAGS_nvidia_smi = "";
  EXPECT_EQ(std::vector<std::string>{}, queryNvidiaSmi("foo", {"bar"}, 10));
  FLAGS_nvidia_smi = "a";
  EXPECT_THROW(queryNvidiaSmi("foo", {"bar"}, 1000), std::runtime_error);
  EXPECT_EQ(
    std::vector<std::string>{"borf\n"},
    queryNvidiaSmi("foo", {"bar", "baz"}, 1000)
  );
}

TEST(TestCheckedSplitLine, SplitLine) {
  // Splits and removes \n
  using SPVec = std::vector<folly::StringPiece>;
  EXPECT_EQ((SPVec{""}), checkedSplitLine(";", "", 1));
  EXPECT_EQ((SPVec{"a"}), checkedSplitLine(";", "a", 1));
  EXPECT_EQ((SPVec{"a", "b", "cde"}), checkedSplitLine("; ", "a; b; cde", 3));
  EXPECT_EQ((SPVec{"f", "gh"}), checkedSplitLine("/", "f/gh\n", 2));
  // Throws if the part count does not match
  EXPECT_THROW(checkedSplitLine("/", "f/gh\n", 1), std::runtime_error);
  EXPECT_THROW(checkedSplitLine("; ", "a; b; cde", 4), std::runtime_error);
}
