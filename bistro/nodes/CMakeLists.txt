add_library(
  bistro_nodes STATIC
  NodeFetcher.cpp
  Nodes.cpp
  NodesLoader.cpp
  RangeLabelFetcher.cpp
  ManualFetcher.cpp
  utils.cpp
  ScriptFetcher.cpp
)
bistro_link_libraries(
  bistro_nodes
  bistro_config
  bistro_cron
  bistro_utils
)

add_subdirectory(test)
