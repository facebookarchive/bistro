add_library(
  bistro_cron_utils STATIC
  date_time.cpp
)
bistro_link_libraries(
  bistro_cron_utils
)

add_subdirectory(test)
