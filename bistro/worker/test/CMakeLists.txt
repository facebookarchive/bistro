add_library(
  lib_bistro_worker_test STATIC
  BistroWorkerTestThread.cpp
  FakeBistroWorkerThread.cpp
)
bistro_link_libraries(
  lib_bistro_worker_test
  lib_bistro_server_test
  lib_bistro_worker
)

add_gtest(test_worker lib_bistro_worker_test)
