# Do NOT use this directly, run build/build.sh instead.

cmake_minimum_required(VERSION 2.8)
include("build/setup.cmake")  # Hides the setup details

add_subdirectory(config)
add_subdirectory(cron)
add_subdirectory(flags)
add_subdirectory(if)
add_subdirectory(monitor)
add_subdirectory(nodes)
add_subdirectory(remote)
add_subdirectory(runners)
add_subdirectory(scheduler)
add_subdirectory(server)
add_subdirectory(sqlite)
add_subdirectory(statuses)
add_subdirectory(thrift)
add_subdirectory(utils)
add_subdirectory(worker)

add_library(
  bistro_lib STATIC
  Bistro.cpp
)
bistro_link_libraries(
  bistro_lib
  bistro_config
  bistro_cron
  bistro_if
  bistro_monitor
  bistro_nodes
  bistro_runners
  bistro_scheduler
  bistro_server
  bistro_sqlite
  bistro_statuses
  bistro_utils
)

add_executable(bistro server/main.cpp)
target_link_libraries(bistro bistro_lib)

add_executable(bistro_worker worker/main.cpp)
target_link_libraries(bistro_worker bistro_worker_lib)

add_executable(bistro_benchmark benchmark/db_scraping_benchmark.cpp)
target_link_libraries(
  bistro_benchmark
  bistro_lib
  libfollybenchmark.so
)
