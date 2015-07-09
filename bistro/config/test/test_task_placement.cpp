/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/utils.h"
#include "bistro/bistro/config/Node.h"

using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestJob, HandleWorkerLocality) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    ("nodes", dynamic::object
      ("levels", {"level1", "level2"})
      ("node_source", "range_label")
    )
  );
  Node host1("host1", 1, true);
  Node db1("db1", 2, true, &host1);

  {
    dynamic d = dynamic::object("owner", "owner");
    Job j(c, "job_name", d);
    EXPECT_TRUE(j.requiredHostForTask(db1).empty());
  }

  {
    dynamic d = dynamic::object
      ("owner", "owner")
      ("level_for_host_placement", "level1");
    Job j(c, "job_name", d);
    EXPECT_EQ("host1", j.requiredHostForTask(db1));
  }

  {
    dynamic d = dynamic::object
      ("owner", "owner")
      ("host_placement", "host2");
    Job j(c, "job_name3", d);
    EXPECT_EQ("host2", j.requiredHostForTask(db1));
  }
}
