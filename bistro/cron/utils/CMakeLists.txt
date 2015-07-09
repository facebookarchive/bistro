add_library(
  lib_bistro_cron_utils STATIC
  date_time.cpp
)
bistro_link_libraries(
  lib_bistro_cron_utils
)

add_subdirectory(test)
