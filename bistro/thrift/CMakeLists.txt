add_library(
  lib_bistro_thrift STATIC
  ThriftConversion.cpp
)
bistro_link_libraries(lib_bistro_thrift lib_bistro_if)

add_gtest(test_thrift_conversion lib_bistro_thrift)
