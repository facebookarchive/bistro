add_library(
  lib_bistro_remote STATIC
  RemoteWorkers.cpp
  RemoteWorker.cpp
  RemoteWorkerState.cpp
  RemoteWorkerSelector.cpp
)
bistro_link_libraries(
  lib_bistro_remote
  lib_bistro_flags
  lib_bistro_statuses
)

add_subdirectory(test)
