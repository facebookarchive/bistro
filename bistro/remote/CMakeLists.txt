add_library(
  bistro_remote STATIC
  RemoteWorkers.cpp
  RemoteWorker.cpp
  RemoteWorkerState.cpp
  RemoteWorkerSelector.cpp
)
bistro_link_libraries(
  bistro_remote
  bistro_flags
  bistro_statuses
)

add_subdirectory(test)
