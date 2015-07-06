add_library(
  bistro_worker_test STATIC
  BistroWorkerTestThread.cpp
  FakeBistroWorkerThread.cpp
)
bistro_link_libraries(
  bistro_worker_test
  bistro_server_test
  bistro_worker_lib
)

add_gtest(test_worker bistro_worker_test)
