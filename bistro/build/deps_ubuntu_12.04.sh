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
# Installs Bistro's dependencies to /usr/local on a clean Ubuntu 12.04 x64
# system.  Primarily intended for Travis CI, since most engineers don't run
# distributions this stale.
#
# WARNING: Uses 'sudo' to upgrade your system with impunity:
#  - Adds several PPAs for missing/outdated dependencies
#  - Installs several from-source dependencies in /usr/local
#
# Library sources & build files end up in bistro/bistro/build/deps.
#

set -ex

BUILD_DIR="$(readlink -f "$(dirname "$0")")"
mkdir -p "$BUILD_DIR/deps"
cd "$BUILD_DIR/deps"

# TODO(#7640974): Remove these once the wangle github repo is unbroken. 
# `git config` is needed so that `git am` works, which is needed so that I
# can use my newfangled build code with older, working folly & fbthrift.
git config --global user.email "you@example.com"
git config --global user.name "Your Name"
cp "$BUILD_DIR/folly-build.patch" .  # fbthrift-build.patch looks for it here

sudo apt-get install -y git

# fbthrift -- also pulls in folly, an a ton of other dependencies.
pushd .
git clone https://github.com/facebook/fbthrift     
cd fbthrift/thrift
FBTHRIFT_VERSION="$(cat "$BUILD_DIR"/FBTHRIFT_VERSION)"  # own line for set -e
git checkout "$FBTHRIFT_VERSION"
# TODO(#7640974): Remove once the wangle github repo is unbroken.
git am "$BUILD_DIR/fbthrift-build.patch"
./build/deps_ubuntu_12.04.sh 
autoreconf -if
./configure
make
sudo make install
sudo ldconfig
popd

# build.sh needs unzip to install gtest.
sudo apt-get install -y unzip libsqlite3-dev
