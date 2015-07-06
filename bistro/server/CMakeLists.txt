# This line is here to appease update_cmake.sh -- this is the main binary:
#   main.cpp
add_library(
  bistro_server STATIC
  Connection.cpp
  HTTPServer.cpp
  HTTPMonitor.cpp
  ThriftMonitor.cpp
)

bistro_link_libraries(
  bistro_server
  bistro_config
  bistro_if
  bistro_thrift
)

add_subdirectory(test)
