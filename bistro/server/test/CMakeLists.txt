add_library(
  bistro_server_test STATIC
  ThriftMonitorTestThread.cpp
)
bistro_link_libraries(
  bistro_server_test
  bistro_if
  bistro_monitor
  bistro_runners
  bistro_server
  bistro_statuses
)

add_gtest(test_thrift_monitor bistro_server_test)
