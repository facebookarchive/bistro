add_library(
  lib_bistro_sqlite STATIC
  Database.cpp
  Statement.cpp
)
bistro_link_libraries(
  lib_bistro_sqlite
)

add_gtest(test_sqlite lib_bistro_sqlite)
