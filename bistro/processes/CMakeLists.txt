add_library(
  lib_bistro_processes STATIC
  AsyncReadPipeRateLimiter.cpp
  TaskSubprocessQueue.cpp
)
bistro_link_libraries(
  lib_bistro_processes
  lib_bistro_statuses
  lib_bistro_utils
)

add_subdirectory(tests)
