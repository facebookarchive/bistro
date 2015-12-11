add_library(
  lib_bistro_physical STATIC
  AllTasksPhysicalResourceMonitor.cpp
  CGroupResources.cpp
  TaskPhysicalResourceFetcher.cpp
  UsablePhysicalResourceFetcher.cpp
  UsablePhysicalResourceMonitor.cpp
  utils.cpp
)

bistro_link_libraries(
  lib_bistro_physical
)

add_subdirectory(test)
