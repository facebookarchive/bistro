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

if ! "$BUILD_DIR/update_cmake.sh" ; then
  echo "Please update CMake files before building"
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
    wget https://googletest.googlecode.com/files/gtest-1.7.0.zip \
      -O gtest-1.7.0.zip
    if [[ "$(sha1sum gtest-1.7.0.zip  | cut -f 1 -d\ )" != \
          "f85f6d2481e2c6c4a18539e391aa4ea8ab0394af" ]] ; then
      echo "Invalid SHA1 checksum for gtest-1.7.0.zip" 1>&2
      exit 1
    fi
    # On error, don't leave a partial directory. CMake takes care of the rest.
    unzip gtest-1.7.0.zip || rm -r gtest-1.7.0
  fi
  popd
}

fetch_gtest

# Hacky way to build Thrift until we have a proper CMake module to do it.
cd "$BUILD_DIR/.."
update_thrift if if/*.thrift build/fbinclude/common/fb303/if/fb303.thrift 

D="$BUILD_DIR/$1"
mkdir -p "$D"
cd "$D"

# Re-run every time, since we use a glob to find Thrift *.cpp files.
cmake -DCMAKE_BUILD_TYPE="$1" ../..
make
if [[ "$2" == "runtests" ]] ; then 
  ctest
fi
