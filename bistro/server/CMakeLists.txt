# This line is here to appease update_cmake.sh -- this is the main binary:
#   main.cpp
add_library(
  lib_bistro_server STATIC
  Connection.cpp
  HTTPServer.cpp
  HTTPMonitor.cpp
  ThriftMonitor.cpp
)

bistro_link_libraries(
  lib_bistro_server
  lib_bistro_config
  lib_bistro_if
  lib_bistro_thrift
)

add_subdirectory(test)
