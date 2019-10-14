# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Some of these files might be unused, but it's easier to build everything.
file(GLOB lib_fb303_if_SRC "../cmake/fbinclude/common/fb303/if/gen-cpp2/*.cpp")
file(GLOB lib_bistro_if_SRC "gen-cpp2/*.cpp")
add_library(lib_bistro_if STATIC ${lib_bistro_if_SRC} ${lib_fb303_if_SRC})
bistro_link_libraries(lib_bistro_if)
