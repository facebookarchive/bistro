/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <string>

namespace facebook { namespace bistro {

// TODO: Maybe rename this TaskStatusFile, because this is not currently a
// general-purpose tempfile.  For example, readline() is length-limited().
class TemporaryFile {

public:
  explicit TemporaryFile(const boost::filesystem::path& path = "/tmp");
  ~TemporaryFile();

  TemporaryFile(TemporaryFile&&) = default;
  TemporaryFile& operator=(TemporaryFile&&) = default;

  const boost::filesystem::path& getFilename() const;

  void writeString(const std::string& s) const;

  std::string readline() const;  // limited to 4096 bytes (see cpp)

private:
  boost::filesystem::path filename_;

};

class TemporaryDir {

public:
  explicit TemporaryDir(const boost::filesystem::path& path = "/tmp");
  ~TemporaryDir();

  TemporaryDir(TemporaryDir&&) = default;
  TemporaryDir& operator=(TemporaryDir&&) = default;

  const boost::filesystem::path& getPath() const;

  TemporaryFile createFile() const;

private:
  boost::filesystem::path path_;

};

}}
