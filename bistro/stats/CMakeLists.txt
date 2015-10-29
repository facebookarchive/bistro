add_library(
  lib_bistro_stats STATIC
  SubprocessStats.cpp
  SubprocessStatsSigarGetter.cpp
)

bistro_link_libraries(
  lib_bistro_stats
)

add_subdirectory(test)
