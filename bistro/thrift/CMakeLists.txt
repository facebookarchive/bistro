add_library(
  bistro_thrift STATIC
  ThriftConversion.cpp
)
bistro_link_libraries(bistro_thrift bistro_if)

add_gtest(test_thrift_conversion bistro_thrift)
