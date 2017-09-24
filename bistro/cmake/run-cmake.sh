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
# Runs `cmake` for Bistro, directing output to cmake/Release/ or cmake/Debug/.
#
# The last line printed to stdout is the path where you can run `make`, so
# this script can be used e.g. as follows:
#
#   cd "$(./run-cmake.sh Debug ... | tee /proc/self/fd/2 | tail)" && make
#

set -e

build_dir="$(readlink -f "$(dirname "$0")")"

if [[ "$1" != "Release" && "$1" != "Debug" ]] ; then
  echo "Usage: $0 <Release|Debug> [CMake args]" 1>&2
  exit 1
fi
build_type=$1
shift 1
# The remaining arguments will be passed to CMake

# TODO: This neither deletes generated files nor auto-updates CMakeLists.txt
update_thrift() {
  echo "Generating Thrift Files"
  shift 1
  BISTRO_HOME="$build_dir/../../.."
  pushd $BISTRO_HOME
  for f in "$@" ; do
    thrift1 -o "bistro/bistro/$(dirname $f)" \
      --gen mstch_cpp2:stack_arguments \
      --templates $TEMPLATES_PATH \
      -I "$BISTRO_HOME" -I "$build_dir/fbinclude" "bistro/bistro/$f"
  done
  popd
}

fetch_gtest() {
  mkdir -p "$build_dir/deps"
  pushd "$build_dir/deps"
  if [[ ! -d gtest-1.7.0 ]] ; then
    wget https://github.com/google/googletest/archive/release-1.7.0.tar.gz \
      -O googletest-release-1.7.0.tgz
    if [[ "$(sha1sum googletest-release-1.7.0.tgz  | cut -f 1 -d\ )" != \
          "d7aa4b8536f3a007b480cf016be8a4333dbf4768" ]] ; then
      echo "Invalid SHA1 checksum for googletest-release-1.7.0.tgz" 1>&2
      exit 1
    fi
    # On error, don't leave a partial directory. CMake takes care of the rest.
    tar xzf googletest-release-1.7.0.tgz || rm -r googletest-release-1.7.0
    mv googletest-release-1.7.0 gtest-1.7.0
  fi
  popd
}

fetch_gtest

# Hacky way to build Thrift until we have a proper CMake module to do it.
cd "$build_dir/.."
update_thrift if if/*.thrift cmake/fbinclude/common/fb303/if/fb303.thrift
./cmake/targets_to_cmake_lists.py .

# Put our artifacts in cmake/{Debug,Release}
make_dir="$build_dir/$build_type"
mkdir -p "$make_dir"
cd "$make_dir"

# Re-run every time, since we use a glob to find Thrift *.cpp files.
cmake -DCMAKE_BUILD_TYPE="$build_type" "$@" ../..

cat <<EOF
Bistro's cmake finished successfully. You can now change into the following
directory, and run "make":

$make_dir
EOF
