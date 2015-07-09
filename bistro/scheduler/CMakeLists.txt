add_library(
  lib_bistro_scheduler STATIC
  RankedPrioritySchedulerPolicy.cpp
  RandomizedPrioritySchedulerPolicy.cpp
  RoundRobinSchedulerPolicy.cpp
  LongTailSchedulerPolicy.cpp
  Scheduler.cpp
  SchedulerPolicy.cpp
  utils.cpp
)
bistro_link_libraries(
  lib_bistro_scheduler
  lib_bistro_config
  lib_bistro_flags
  lib_bistro_if
  lib_bistro_utils
  lib_bistro_nodes
  lib_bistro_statuses
)

add_subdirectory(test)
