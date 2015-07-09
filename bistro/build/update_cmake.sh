#!/bin/bash
#
# Copyright (c) 2015, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
#

#
# Run this to help you keep Bistro's CMakeLists.txt up to date.
#

cd $(dirname $(dirname $0))
_UPDATE_CMAKE_EXIT_CODE=0

# Adds unnecessary dependencies for header-only tests, but that's fine.
function add_test_to_cmake() {
  l=$(grep '^add_gtest(' "$2" | tail -n 1)
  if [[ "$l" == "" ]]; then
    echo " - Please manually add 'add_gtest($1 bistro_some_lib)' to $2" 1>&2
    export _UPDATE_CMAKE_EXIT_CODE=1
  else
    echo "$l" | \
      sed "s/add_gtest(\(test_[^ ]*\) /add_gtest($1 /" >> "$2"
  fi
}

function add_source_to_cmake() {
  l=$(grep '.cpp$' "$2" | tail -n 1)
  if [[ "$l" == "" ]]; then
    cat 1>&2 <<EOF
 - Please manually add this to $2:

add_library(
  bistro_some_lib STATIC
  $1
)
bistro_link_libraries(
  bistro_some_lib
)

   NOTE: You may need other changes, like add_subdirectory(...) in the 
         parent CMakeLists.txt, or extra dependencies in the various
         bistro_link_libraries.

EOF
    export _UPDATE_CMAKE_EXIT_CODE=1
  else
    sed -i "s/^$l$/\0\n  $1/" "$2"
  fi
}

for f in $(git ls-files '*.cpp'); do 
  cmake_file=$(dirname "$f")/CMakeLists.txt; 
  if [[ ! -r "$cmake_file" ]] ; then 
    touch "$cmake_file"
    git add "$cmake_file"
  fi
  f_base=$(basename $f)
  if ! (
    grep -q "  $f_base" "$cmake_file" ||
    grep -q "^add_gtest(${f_base%.cpp} " "$cmake_file"
  ) ; then 
    if [ "${f_base#test_}" == "$f_base" ] ; then
      add_source_to_cmake "$f_base" "$cmake_file"
    else
      add_test_to_cmake "${f_base%.cpp}" "$cmake_file"
    fi
  fi
done

exit ${_UPDATE_CMAKE_EXIT_CODE}
