/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/config/Node.h"
#include "bistro/bistro/config/parsing_common.h"

using namespace facebook::bistro;
using namespace folly;
using namespace std;

TEST(TestJob, HandleWorkerLocality) {
  Config c(dynamic::object
    ("resources", dynamic::object)
    (kNodes, dynamic::object
      ("levels", dynamic::array("level1", "level2"))
      ("node_sources", dynamic::array(
        dynamic::object("source", "range_label")
      ))
    )
  );
  Node host1("host1", 1, true);
  Node db1("db1", 2, true, &host1);

  {
    dynamic d = dynamic::object("owner", "owner");
    Job j(c, "job_name1", d);
    EXPECT_TRUE(j.requiredHostForTask(db1).empty());
  }

  {
    dynamic d = dynamic::object
      ("owner", "owner")
      ("level_for_host_placement", "level1");
    Job j(c, "job_name2", d);
    EXPECT_EQ("host1", j.requiredHostForTask(db1));
  }

  {
    dynamic d = dynamic::object
      ("owner", "owner")
      ("host_placement", "host2");
    Job j(c, "job_name3", d);
    EXPECT_EQ("host2", j.requiredHostForTask(db1));
  }

  // If both are specified, the more specific "host_placement" wins.
  {
    dynamic d = dynamic::object
      ("owner", "owner")
      ("level_for_host_placement", "level1")
      ("host_placement", "host3");
    Job j(c, "job_name4", d);
    EXPECT_EQ("host3", j.requiredHostForTask(db1));
  }
}
