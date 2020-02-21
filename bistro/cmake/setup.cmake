# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Do NOT use this -- run cmake/run-cmake.sh instead & read its docblock.

include_directories(
  # Our includes start with "bistro/bistro/"
  "${PROJECT_SOURCE_DIR}/../.."
  # A hack to include a stub for some FB-specific includes under "common/".
  "${PROJECT_SOURCE_DIR}/cmake/fbinclude"
  "${CMAKE_INSTALL_PREFIX}/include"
)

link_directories("${CMAKE_INSTALL_PREFIX}/lib")

# We generally need to track folly here, or the build may break.  E.g.
# having `c++14` here became incompatible with folly built with `gnu++1z`.
add_definitions(-std=gnu++1z -Wno-deprecated)

set(
  BISTRO_LINK_DEPS
  libfolly.so
  libfmt.so
  libglog.so
  libgflags.so
  libboost_context.so
  libboost_date_time.so
  libboost_regex.so
  libboost_system.so
  libboost_thread.so
  libboost_filesystem.so
  libdouble-conversion.so
  libproxygenhttpserver.so
  libproxygen.so
  libcrypto.so
  libfizz.so
  libpthread.so
  libsqlite3.so
  libwangle.so
  libssl.so
  libsodium.so
  libz.so
  libzstd.so

  # Thrift comes in a bajillion tiny pieces :'(
  libasync.so
  libconcurrency.so
  libprotocol.so
  libthrift-core.so
  libthriftcpp2.so
  libthriftmetadata.so
  libthriftprotocol.so
  libtransport.so
)

# Use this instead of target_link_libraries() because pretty much everything
# depends on these libraries.  If CMake does not know about the
# dependencies, it is might use the wrong link order, and then you'll see
# mysterious errors like "DSO missing from command line".
#
# TODO: Some of the above dependencies, like SQLite and libthrift, are only
# used by a few modules.  Consider splitting them out?
macro(bistro_link_libraries name)
  target_link_libraries(
    ${name}
    ${ARGN}
    ${BISTRO_LINK_DEPS}
  )
endmacro(bistro_link_libraries)

add_subdirectory(cmake/deps/gtest-1.8.1)
enable_testing()
include_directories("${gtest_SOURCE_DIR}/include" "${gtest_SOURCE_DIR}")

add_library(
  folly_gtest_main STATIC
  folly_gtest_main.cpp
)

macro(add_gtest name)
  add_executable(${name} ${name}.cpp)
  bistro_link_libraries(
    ${name}
    gtest
    folly_gtest_main
    ${ARGN}
  )
  add_test(${name} ${name})
endmacro(add_gtest)
