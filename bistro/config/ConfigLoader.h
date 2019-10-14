/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <memory>

namespace folly {
  class dynamic;
}

namespace facebook { namespace bistro {

class Config;

/**
 * Loads a Config from some data provider. getDataOrThrow() returns an
 * immutable snapshot of the config (e.g. to use while scheduling), or
 * throws indicating that the config is temporarily unavailable.
 *
 * Also, somewhat accidentally, the loader provides mutator methods for
 * changing the config backing store (not available in all implementations).
 * Note that the mutators do **not** instantly update the config returned by
 * getDataOrThrow, since most of our config sources have propagation &
 * polling delays.
 */
class ConfigLoader {
public:
  virtual ~ConfigLoader() {}

  // Must be thread-safe
  virtual std::shared_ptr<const Config> getDataOrThrow() const = 0;

  void saveJob(const std::string&, const folly::dynamic&);
  void deleteJob(const std::string&);

protected:
  virtual void saveJobImpl(const std::string&, const folly::dynamic&) = 0;
  virtual void deleteJobImpl(const std::string&) = 0;
};

}}
