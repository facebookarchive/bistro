add_library(
  bistro_monitor STATIC
  Monitor.cpp
)
bistro_link_libraries(
  bistro_monitor
  bistro_config
  bistro_flags
  bistro_nodes
  bistro_statuses
  bistro_utils
)
