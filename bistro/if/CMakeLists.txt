# Some of these files might be unused, but it's easier to build everything.
file(GLOB lib_bistro_if_SRC "gen-cpp2/*.cpp")
add_library(lib_bistro_if STATIC ${lib_bistro_if_SRC})
bistro_link_libraries(lib_bistro_if)
