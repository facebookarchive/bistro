# Some of these files might be unused, but it's easier to build everything.
file(GLOB bistro_if_SRC "gen-cpp2/*.cpp")
add_library(bistro_if STATIC ${bistro_if_SRC})
bistro_link_libraries(bistro_if)
