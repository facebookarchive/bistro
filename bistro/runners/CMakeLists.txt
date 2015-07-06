add_library(
  bistro_runners STATIC
  BenchmarkRunner.cpp
  LocalRunner.cpp
  NoOpRunner.cpp
  RemoteWorkerRunner.cpp
  TaskRunner.cpp
)
bistro_link_libraries(
  bistro_runners
  bistro_config
  bistro_if
  bistro_nodes
  bistro_remote
  bistro_statuses
  bistro_utils
)

add_subdirectory(test)
