#include <gtest/gtest.h>
#include <vector>

#include "bistro/bistro/utils/hostname.h"

using namespace facebook::bistro;
using namespace std;

TEST(TestUtils, HandleStartsWithAny) {
  vector<string> v = { "foo", "bar" };
  EXPECT_TRUE(startsWithAny("foo123", v));
  EXPECT_TRUE(startsWithAny("bar123", v));
  EXPECT_FALSE(startsWithAny("moooo", v));
  EXPECT_FALSE(startsWithAny("foo", vector<string>{}));
}
