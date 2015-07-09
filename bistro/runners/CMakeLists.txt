add_library(
  lib_bistro_runners STATIC
  BenchmarkRunner.cpp
  LocalRunner.cpp
  NoOpRunner.cpp
  RemoteWorkerRunner.cpp
  TaskRunner.cpp
)
bistro_link_libraries(
  lib_bistro_runners
  lib_bistro_config
  lib_bistro_if
  lib_bistro_nodes
  lib_bistro_remote
  lib_bistro_statuses
  lib_bistro_utils
)

add_subdirectory(test)
