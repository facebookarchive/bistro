#
# Copyright (c) 2015, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
#

# Do NOT use this -- run build/run-cmake.sh instead & read its docblock.

include_directories(
  # Our includes start with "bistro/bistro/"
  "${PROJECT_SOURCE_DIR}/../.."
  # A hack to include a stub for some FB-specific includes under "common/".
  "${PROJECT_SOURCE_DIR}/build/fbinclude"
  "${CMAKE_INSTALL_PREFIX}/include"
)

link_directories("${CMAKE_INSTALL_PREFIX}/lib")

add_definitions(-std=c++14 -Wno-deprecated)

set(
  BISTRO_LINK_DEPS
  libcrypto.so
  libfolly.so
  libglog.so
  libgflags.so
  libboost_date_time.so
  libboost_regex.so
  libboost_system.so
  libboost_thread.so
  libboost_filesystem.so
  libdouble-conversion.so
  libproxygenhttpserver.so
  libpthread.so
  libsqlite3.so
  libthrift.so
  libthriftcpp2.so
  libthriftprotocol.so
  libwangle.so
  libz.so
  libzstd.so
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

add_subdirectory(build/deps/gtest-1.7.0)
enable_testing()
include_directories("${gtest_SOURCE_DIR}/include" "${gtest_SOURCE_DIR}")

macro(add_gtest name)
  add_executable(${name} ${name}.cpp)
  bistro_link_libraries(
    ${name}
    gtest
    gtest_main
    ${ARGN}
  )
  add_test(${name} ${name})
endmacro(add_gtest)
