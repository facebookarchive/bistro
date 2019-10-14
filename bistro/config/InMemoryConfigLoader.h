/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "bistro/bistro/config/Config.h"
#include "bistro/bistro/config/ConfigLoader.h"
#include <folly/dynamic.h>

namespace facebook { namespace bistro {

class InMemoryConfigLoader : public ConfigLoader {
public:
  explicit InMemoryConfigLoader(const folly::dynamic& d)
    : config_(std::make_shared<Config>(d)) {}
  explicit InMemoryConfigLoader(const Config& c)
    : config_(std::make_shared<Config>(c)) {}

  std::shared_ptr<const Config> getDataOrThrow() const override {
    if (config_) {
      return config_;
    }
    throw std::runtime_error(exceptionMessage_);
  }

  void setConfig(const folly::dynamic& d) { config_.reset(new Config(d)); }
  void setConfig(const Config& c) { config_.reset(new Config(c)); }
  void setException(std::string what) {
    config_.reset();
    exceptionMessage_ = std::move(what);
  }

private:
 void deleteJobImpl(const std::string& /*name*/) override {
   throw BistroException("deleteJob not implemented in InMemoryConfigLoader");
  }

  void saveJobImpl(const std::string& /*name*/, const folly::dynamic& /*d*/)
      override {
    throw BistroException("saveJob not implemented in InMemoryConfigLoader");
  }

  std::shared_ptr<const Config> config_;
  std::string exceptionMessage_;
};

}}
