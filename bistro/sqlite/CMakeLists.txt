add_library(
  bistro_sqlite STATIC
  Database.cpp
  Statement.cpp
)
bistro_link_libraries(
  bistro_sqlite
)

add_gtest(test_sqlite bistro_sqlite)
