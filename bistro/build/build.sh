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
# Builds Bistro and tests (and optionally runs tests) in
#   build/Release/ or build/Debug/
#

set -e

BUILD_DIR="$(readlink -f "$(dirname "$0")")"

if [[ "$1" != "Release" && "$1" != "Debug" ]] ; then
  echo "Usage: $0 <Release|Debug> [runtests]" 1>&2
  exit 1
fi

# Does not change the timestamp if the file has not changed.
update_file() {
  if [[ ! -r "$2" ]] ; then
    mkdir -p "$(dirname "$2")"
    echo "Making $(readlink -f "$2")"
    mv "$1" "$2"
  elif ! diff -q "$1" "$2" ; then
    echo "Updating $(readlink -f "$2")"
    mv "$1" "$2"
  # else ; echo "Already up-to-date: $(readlink -f "$2")"
  fi
}

# TODO: This neither deletes generated files nor auto-updates CMakeLists.txt
update_thrift() {
  echo "Checking if Thrift-generated sources changed"
  OUT_DIR="$1"
  shift 1
  TMP_DIR="$(mktemp -d)"
  for f in "$@" ; do
    python -mthrift_compiler.main -o "$TMP_DIR" --gen cpp2:stack_arguments \
      -I ../.. -I "$BUILD_DIR/fbinclude" "$f"
  done
  for f in "$TMP_DIR/gen-cpp2"/* ; do
    update_file "$f" "$OUT_DIR${f#$TMP_DIR}"
  done
  rm -r "$TMP_DIR/gen-cpp2"
  rmdir "$TMP_DIR"
}

fetch_gtest() {
  mkdir -p "$BUILD_DIR/deps"
  pushd "$BUILD_DIR/deps"
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
cd "$BUILD_DIR/.."
update_thrift if if/*.thrift build/fbinclude/common/fb303/if/fb303.thrift
./build/targets_to_cmake_lists.py .

# Start in e.g. build/Debug/build, and so we can put the artifacts in Debug/.
D="$BUILD_DIR/$1/build"
mkdir -p "$D"
cd "$D"

# Re-run every time, since we use a glob to find Thrift *.cpp files.
cmake -DCMAKE_BUILD_TYPE="$1" ../..
make
if [[ "$2" == "runtests" ]] ; then
  ctest
fi
