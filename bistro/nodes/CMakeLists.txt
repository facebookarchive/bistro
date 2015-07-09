add_library(
  lib_bistro_nodes STATIC
  NodeFetcher.cpp
  Nodes.cpp
  NodesLoader.cpp
  RangeLabelFetcher.cpp
  ManualFetcher.cpp
  utils.cpp
  ScriptFetcher.cpp
)
bistro_link_libraries(
  lib_bistro_nodes
  lib_bistro_config
  lib_bistro_cron
  lib_bistro_utils
)

add_subdirectory(test)
