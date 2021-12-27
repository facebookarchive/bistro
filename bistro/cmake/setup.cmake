# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Do NOT use this -- run cmake/run-cmake.sh instead & read its docblock.

set(CMAKE_MODULE_PATH
  # Shared modules to find packages
  "${PROJECT_SOURCE_DIR}/../build/fbcode_builder/CMake"
  # Custom modules to find packages
  "${PROJECT_SOURCE_DIR}/cmake/deps"
  ${CMAKE_MODULE_PATH}
)

include_directories(
  # Our includes start with "bistro/bistro/"
  "${PROJECT_SOURCE_DIR}/../.."
  # A hack to include a stub for some FB-specific includes under "common/".
  "${PROJECT_SOURCE_DIR}/cmake/fbinclude"
  "${CMAKE_INSTALL_PREFIX}/include"
)

# Future: update the Buck -> CMake script to resolve deps to the below
# instead of linking everything to everything?
#
# Keep the non-Boost dependencies alphabetical, and update `bistro_deps` below.
#
# IMPORTANT: It appears that as of 10/2020, finding Boost does not work well
# with `find_dependency`.  Namely, the next line runs first, and all the
# other components that are e.g. requested by `proxygen` are never found.
# So we have to maintain our component list as a superset of everything that
# our dependencies use.  Please annotate these with "<project> superset hack".
find_package(Boost 1.58 REQUIRED
  COMPONENTS
    context
    date_time
    filesystem
    iostreams  # `proxygen` superset hack, see above
    regex
    system
    thread
)
find_package(FBThrift CONFIG REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(Gflags REQUIRED)  # From the common CMake/
find_package(Glog REQUIRED)  # From the common CMake/
find_package(proxygen CONFIG REQUIRED)
find_package(Sqlite3 REQUIRED)  # From Eden
find_package(Threads REQUIRED)  # Standard module
find_package(wangle CONFIG REQUIRED)
find_package(GTest REQUIRED)
# Bistro does not directly depend on this, but Proxygen does.  Remove this
# line, and the corresponding `FindZstd.cmake` once Proxygen & Folly use a
# standard (and inheritable) method of discovering `zstd`.
find_package(Zstd)

# Do this after "find_package" calls to avoid shenanigans from triggering
# third-party modules.
#
# We generally need to track folly here, or the build may break.  E.g.
# having `gnu++14` here became incompatible with folly built with `gnu++1z`.
#
# An example failure due to having C++14 is like so:
#   undefined reference to `folly::ThreadedRepeatingFunctionRunner::add(
#     std::__cxx11::basic_string<char,
#         std::char_traits<char>, std::allocator<char> >,
#     folly::Function<std::chrono::duration<long, std::ratio<1l, 1000l> > ()>,
#     std::chrono::duration<long, std::ratio<1l, 1000l> >)'
# This is because the `Function` is declared as `noexcept` in `folly`, and
# as of C++17, this annotation is part of the ABI.  But if we compile with
# C++14, it looks for an unannotated overload.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(bistro_deps INTERFACE)
target_link_libraries(bistro_deps
  # The dependencies should follow the `find_package` order
  INTERFACE
    Boost::context
    Boost::date_time
    Boost::filesystem
    Boost::regex
    Boost::system
    Boost::thread
    FBThrift::thriftcpp2
    Folly::folly
    ${LIBGFLAGS_LIBRARY}
    ${GLOG_LIBRARIES}
    proxygen::proxygen
    proxygen::proxygenhttpserver
    ${SQLITE3_LIBRARY}
    Threads::Threads
    wangle::wangle
    ${GTEST_LIBRARIES}
)

# Future: kill this, just link `bistro_deps` directly.
#
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
    bistro_deps
  )
  target_include_directories(
    ${name}
    PUBLIC
      ${SQLITE3_INCLUDE_DIR}
  )
endmacro(bistro_link_libraries)

enable_testing()
add_library(gtest INTERFACE)
target_include_directories(gtest INTERFACE ${GTEST_INCLUDE_DIRS})
target_link_libraries(gtest INTERFACE ${GTEST_LIBRARIES})

add_library(
  folly_gtest_main STATIC
  folly_gtest_main.cpp
)
target_include_directories(
  folly_gtest_main PUBLIC
  ${GTEST_INCLUDE_DIR}
)
target_link_libraries(folly_gtest_main
  PUBLIC
    bistro_deps
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
