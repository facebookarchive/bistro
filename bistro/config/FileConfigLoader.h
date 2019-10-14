/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <memory>
#include <string>
#include <vector>

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/ConfigLoader.h"
#include "bistro/bistro/utils/PeriodicPoller.h"

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro {

class FileConfigLoader : public ConfigLoader {

public:
  explicit FileConfigLoader(
    std::chrono::milliseconds update_period,
    const boost::filesystem::path& filename
  );

  std::shared_ptr<const Config> getDataOrThrow() const override {
    return loader_.getDataOrThrow();
  }

private:
  // A default-constructible time_t
  struct PollerState {
    PollerState() : modificationTime_(0) {}
    time_t modificationTime_;
  };

  void saveJobImpl(const std::string&, const folly::dynamic&) override;
  void deleteJobImpl(const std::string& name) override;

  static std::shared_ptr<const Config> parseConfigFile(
    std::shared_ptr<const Config> prev_config,
    const std::string&
  );

  PeriodicPoller<
    std::string,  // RawData: file contents
    PollerState,
    Config,
    FileConfigLoader::parseConfigFile
  > loader_;
};

}}
