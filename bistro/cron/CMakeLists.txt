add_library(
  lib_bistro_cron STATIC
  StandardCrontabItem.cpp
  CrontabSelector.cpp
  EpochCrontabItem.cpp
  CrontabItem.cpp
)
bistro_link_libraries(
  lib_bistro_cron
  lib_bistro_cron_utils
)

add_subdirectory(test)
add_subdirectory(utils)
