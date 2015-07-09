add_library(
  lib_bistro_monitor STATIC
  Monitor.cpp
)
bistro_link_libraries(
  lib_bistro_monitor
  lib_bistro_config
  lib_bistro_flags
  lib_bistro_nodes
  lib_bistro_statuses
  lib_bistro_utils
)
