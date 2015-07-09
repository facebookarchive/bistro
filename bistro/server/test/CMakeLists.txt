add_library(
  lib_bistro_server_test STATIC
  ThriftMonitorTestThread.cpp
)
bistro_link_libraries(
  lib_bistro_server_test
  lib_bistro_if
  lib_bistro_monitor
  lib_bistro_runners
  lib_bistro_server
  lib_bistro_statuses
)

add_gtest(test_thrift_monitor lib_bistro_server_test)
