add_library(
  bistro_scheduler STATIC
  RankedPrioritySchedulerPolicy.cpp
  RandomizedPrioritySchedulerPolicy.cpp
  RoundRobinSchedulerPolicy.cpp
  LongTailSchedulerPolicy.cpp
  Scheduler.cpp
  SchedulerPolicy.cpp
  utils.cpp
)
bistro_link_libraries(
  bistro_scheduler
  bistro_config
  bistro_flags
  bistro_if
  bistro_utils
  bistro_nodes
  bistro_statuses
)

add_subdirectory(test)
