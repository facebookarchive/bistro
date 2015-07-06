add_library(
  bistro_config STATIC
  Config.cpp
  FileConfigLoader.cpp
  Job.cpp
  JobBackoffSettings.cpp
  JobFilters.cpp
  Node.cpp
  SchedulerType.cpp
  utils.cpp
  ConfigLoader.cpp
  RemoteWorkerSelectorType.cpp
)
bistro_link_libraries(bistro_config bistro_utils)

add_subdirectory(test)
