# Appease update_cmake.sh:  main.cpp
add_library(
  bistro_worker_lib STATIC
  BistroWorkerHandler.cpp
)
bistro_link_libraries(
  bistro_worker_lib
  bistro_if
  bistro_statuses
  bistro_utils
)

add_subdirectory(test)
