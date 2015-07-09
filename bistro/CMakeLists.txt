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
  lib_bistro STATIC
  Bistro.cpp
)
bistro_link_libraries(
  lib_bistro
  lib_bistro_config
  lib_bistro_cron
  lib_bistro_if
  lib_bistro_monitor
  lib_bistro_nodes
  lib_bistro_runners
  lib_bistro_scheduler
  lib_bistro_server
  lib_bistro_sqlite
  lib_bistro_statuses
  lib_bistro_utils
)

add_executable(bistro_scheduler server/main.cpp)
target_link_libraries(bistro_scheduler lib_bistro)

add_executable(bistro_worker worker/main.cpp)
target_link_libraries(bistro_worker lib_bistro_worker)

add_executable(bistro_benchmark benchmark/db_scraping_benchmark.cpp)
target_link_libraries(
  bistro_benchmark
  lib_bistro
  libfollybenchmark.so
)
