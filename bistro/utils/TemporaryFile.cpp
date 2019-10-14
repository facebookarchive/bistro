/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/TemporaryFile.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <string>

namespace facebook { namespace bistro {

namespace {
  // Allow lines up to 4k; we don't want to read arbitrarily large amounts of
  // data.
  const size_t kMaxLineLength = 4096;
}

TemporaryFile::TemporaryFile(const boost::filesystem::path& path)
    : filename_(path / boost::filesystem::unique_path()) {
}

TemporaryFile::~TemporaryFile() {
  boost::filesystem::remove(filename_);
}

const boost::filesystem::path& TemporaryFile::getFilename() const {
  return filename_;
}

void TemporaryFile::writeString(const std::string& s) const {
  std::ofstream(filename_.native()) << s;
}

std::string TemporaryFile::readline() const {
  char buf[kMaxLineLength]; // Statuses shouldn't be too large
  std::ifstream fin(filename_.native());
  fin.getline(buf, kMaxLineLength);
  return std::string(buf);
}

TemporaryDir::TemporaryDir(const boost::filesystem::path& path)
    : path_(path / boost::filesystem::unique_path()) {
  boost::filesystem::create_directories(path_);
}

TemporaryDir::~TemporaryDir() {
  boost::filesystem::remove_all(path_);
}

const boost::filesystem::path& TemporaryDir::getPath() const {
  return path_;
}

TemporaryFile TemporaryDir::createFile() const {
  return TemporaryFile(path_);
}

}}
