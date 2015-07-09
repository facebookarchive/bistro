# Appease update_cmake.sh:  main.cpp
add_library(
  lib_bistro_worker STATIC
  BistroWorkerHandler.cpp
)
bistro_link_libraries(
  lib_bistro_worker
  lib_bistro_if
  lib_bistro_statuses
  lib_bistro_utils
)

add_subdirectory(test)
