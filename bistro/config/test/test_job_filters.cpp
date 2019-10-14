/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/config/JobFilters.h"
#include "bistro/bistro/config/Node.h"
#include <folly/Conv.h>
#include <folly/dynamic.h>

using namespace facebook::bistro;
using namespace folly;
using namespace std;
using boost::regex;

#define DOES_PASS(filters, name) EXPECT_TRUE(filters.doesPass("", Node(name)))
#define DOESNT_PASS(filters, name) \
  EXPECT_FALSE(filters.doesPass("", Node(name)))

TEST(TestJobFilters, HandleEmpty) {
  JobFilters filters;
  DOES_PASS(filters, "abc");
  DOES_PASS(filters, "abc.123");
  EXPECT_TRUE(filters.isEmpty());
}

TEST(TestJobFilters, HandleNonTriviallyEmpty) {
  JobFilters filters(dynamic::object("fraction_of_nodes", 1.0));
  DOES_PASS(filters, "abc");
  DOES_PASS(filters, "abc.123");
  EXPECT_TRUE(filters.isEmpty());
}

TEST(TestJobFilters, HandleNoFilters) {
  JobFilters filters(dynamic::object());
  DOES_PASS(filters, "abc");
  DOES_PASS(filters, "dbs.123");
}

TEST(TestJobFilters, HandleWhitelist) {
  JobFilters filters(dynamic::object("whitelist", dynamic::array("abc")));
  DOES_PASS(filters, "abc");
  DOESNT_PASS(filters, "dbs.123");
}

TEST(TestJobFilters, HandleWhitelistRegex) {
  JobFilters filters(dynamic::object("whitelist_regex", "dbs.*"));
  DOESNT_PASS(filters, "abc");
  DOES_PASS(filters, "dbs.123");
  DOES_PASS(filters, "dbs.456");
}

TEST(TestJobFilters, HandleBlacklist) {
  JobFilters filters(dynamic::object("blacklist", dynamic::array("abc")));
  DOESNT_PASS(filters, "abc");
  DOES_PASS(filters, "dbs.123");
  DOES_PASS(filters, "dbs.456");
}

TEST(TestJobFilters, HandleBlacklistRegex) {
  JobFilters filters(dynamic::object("blacklist_regex", "dbs.*"));
  DOES_PASS(filters, "abc");
  DOESNT_PASS(filters, "dbs.123");
  DOESNT_PASS(filters, "dbs.456");
}

TEST(TestJobFilters, HandleTagWhitelist) {
  JobFilters filters(dynamic::object
    ("tag_whitelist", dynamic::array("abc", "car"))
  );
  Node no_tags("no_tags");
  Node one_tag("one_tag", 0, false, nullptr, Node::TagSet{"abc"});
  Node two_tags("two_tags", 0, false, nullptr, Node::TagSet{"abc", "car"});
  Node three_tags("three_tags", 0, false, nullptr,
    Node::TagSet{"abc", "car", "ah"});

  EXPECT_FALSE(filters.doesPass("", no_tags));
  EXPECT_TRUE(filters.doesPass("", one_tag));
  EXPECT_TRUE(filters.doesPass("", two_tags));
  EXPECT_TRUE(filters.doesPass("", three_tags));
}

TEST(TestJobFilters, HandleCallback) {
  auto filter_cb = [](const Node& n) -> bool {
    std::vector<std::string> allowed_tags = {"abc", "car"};
    return n.hasTags(allowed_tags);
  };

  JobFilters filters(dynamic::object(), filter_cb);
  Node no_tags("no_tags");
  Node one_tag("one_tag", 0, false, nullptr, Node::TagSet{"abc"});
  Node two_tags("two_tags", 0, false, nullptr, Node::TagSet{"abc", "car"});
  Node three_tags("three_tags", 0, false, nullptr,
    Node::TagSet{"abc", "car", "ah"});

  EXPECT_FALSE(filters.doesPass("", no_tags));
  EXPECT_TRUE(filters.doesPass("", one_tag));
  EXPECT_TRUE(filters.doesPass("", two_tags));
  EXPECT_TRUE(filters.doesPass("", three_tags));
}

TEST(TestJobFilters, HandleCutoff) {
  JobFilters filters(dynamic::object("fraction_of_nodes", 0.5));
  int total_passed = 0;
  for (int i = 0; i < 10000; ++i) {
    if (filters.doesPass("", Node(to<string>(i)))) {
      ++total_passed;
    }
  }
  EXPECT_LT(abs(total_passed - 5000), 100); // within 2 stdev
}

TEST(TestJobFilters, HandleSalting) {
  JobFilters filters(dynamic::object("fraction_of_nodes", 0.5));
  JobFilters filters2(dynamic::object("fraction_of_nodes", 0.25));
  for (int i = 0; i < 10000; ++i) {
    const auto& s = to<string>(i);
    if (filters2.doesPass("salt", Node(s))) {
      EXPECT_TRUE(filters.doesPass("salt", Node(s)));
    }
  }
}

TEST(TestJobFilters, HandleDifferentSalts) {
  // Using filters with two different salts should produce different results.
  JobFilters filters(dynamic::object("fraction_of_nodes", 0.5));
  JobFilters filters2(dynamic::object("fraction_of_nodes", 0.5));
  vector<int> v, v2;
  for (int i = 0; i < 10000; ++i) {
    const auto& s = to<string>(i);
    if (filters.doesPass("salt", Node(s))) {
      v.push_back(i);
    }
    if (filters2.doesPass("salt2", Node(s))) {
      v2.push_back(i);
    }
  }
  EXPECT_NE(v, v2);
}

TEST(TestJobFilters, HandleComparison) {
  dynamic d = dynamic::object
    ("whitelist", dynamic::array("abc", "xyz"))
    ("whitelist_regex", "f.*")
    ("blacklist", dynamic::array("moo"))
    ("blacklist_regex", "y.*")
    ("tag_whitelist", dynamic::array("bah"))
    ("fraction_of_nodes", 0.75)
  ;
  JobFilters f1(d), f2(d);
  EXPECT_EQ(f1, f2);
}

TEST(TestJobFilters, TestEqualityOperator) {
  JobFilters empty_filters;
  dynamic d = dynamic::object
    ("whitelist", dynamic::array("abc", "def"))
    ("whitelist_regex", "cat")
    ("blacklist", dynamic::array("dog"))
    ("blacklist_regex", "foo_dog")
    ("tag_whitelist", dynamic::array("baah"))
    ("fraction_of_nodes", 0.5)
  ;

  dynamic d2 = dynamic::object
    ("whitelist", dynamic::array("abc"))
    ("whitelist_regex", "cat2")
    ("blacklist", dynamic::array("dog2"))
    ("blacklist_regex", "foo_dog2")
    ("tag_whitelist", dynamic::array("baah2"))
    ("fraction_of_nodes", 0.75)
  ;

  JobFilters f1(d), f2(d);
  EXPECT_EQ(f1, f2);
  EXPECT_NE(empty_filters, f1);

  // Changing any field should break equality testing
  for (const auto& pair : d.items()) {
    dynamic d3(d);
    d3[pair.first] = d2[pair.first];
    JobFilters f3(d3);
    EXPECT_NE(f3, f1);
  }

  // These should compare as equal as the second is nontrivially empty;
  JobFilters nontrivially_empty(dynamic::object("fraction_of_nodes", 1.0));
  EXPECT_EQ(empty_filters, nontrivially_empty);
}
