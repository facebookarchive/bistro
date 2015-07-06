add_library(
  bistro_cron STATIC
  StandardCrontabItem.cpp
  CrontabSelector.cpp
  EpochCrontabItem.cpp
  CrontabItem.cpp
)
bistro_link_libraries(
  bistro_cron
  bistro_cron_utils
)

add_subdirectory(test)
add_subdirectory(utils)
