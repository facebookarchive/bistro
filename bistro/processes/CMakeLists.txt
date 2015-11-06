add_library(
  lib_bistro_processes STATIC
  AsyncReadPipeRateLimiter.cpp
)
bistro_link_libraries(
  lib_bistro_processes
)

add_subdirectory(tests)
