/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/config/FileConfigLoader.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/Memory.h>
#include <fstream>

#include "bistro/bistro/config/Job.h"
#include "bistro/bistro/utils/Exception.h"
#include <folly/experimental/AutoTimer.h>

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

namespace {
  const string kSettingsKey = "bistro_settings";
  const string kJobPrefix = "bistro_job->";
}

FileConfigLoader::FileConfigLoader(
    std::chrono::milliseconds update_period,
    const boost::filesystem::path& filename)
    : loader_(
          "FileCfgLoader",
          [filename](
              std::string* out_contents,
              PollerState* state,
              std::shared_ptr<const Config> /*unused_prev_config*/
              ) {
            folly::AutoTimer<> timer;
            time_t cur_time = boost::filesystem::last_write_time(filename);
            if (state->modificationTime_ >= cur_time) {
              timer.log("Config was already fresh");
              return false;
            }
            state->modificationTime_ = cur_time;

            ifstream fin(filename.native());
            stringstream sstr;
            sstr << fin.rdbuf();
            *out_contents = sstr.str();
            timer.log("Read config from ", filename.native());
            return true;
          },
          update_period,
          update_period) {}

std::shared_ptr<const Config> FileConfigLoader::parseConfigFile(
    std::shared_ptr<const Config> prev_config,
    const std::string& contents) {

  folly::AutoTimer<> timer;

  dynamic d = parseJson(contents);

  auto* s = d.get_ptr(kSettingsKey);
  if (!s || !s->isObject()) {
    throw BistroException("Missing bistro_settings property");
  }

  auto config = std::make_shared<Config>(*s);

  for (const auto& pair : d.items()) {
    const string& key = pair.first.asString();
    if (!boost::starts_with(key, kJobPrefix) || !pair.second.isObject()) {
      continue;
    }
    config->addJob(
      std::make_shared<Job>(
          *config,
          key.substr(kJobPrefix.size()),
          pair.second),
      prev_config.get()
    );
  }

  timer.log("Parsed config with ", config->jobs.size(), " jobs");
  return config;
}

void FileConfigLoader::deleteJobImpl(const string& /*name*/) {
  // When you implement this, see the notes for saveJobImpl()
  throw BistroException("Not implemented for files");
}

void FileConfigLoader::saveJobImpl(
    const string& /*name*/,
    const folly::dynamic& /*d*/) {
  // When you implement this, be sure to use the strongest form of locking
  // you can (at a minimum, fcntl FDLOCK) around the read-modify-write.
  //
  //   FDLock fd_lock(filename_);
  //   // It's important to re-read the locked file from disk in case of
  //   // concurrent modifications, by, e.g. other schedulers.
  //   auto c = readConfig();  // Extract this code from refresh()
  //   JobPtr job = make_shared<Job>(c.get(), name, d);
  //   writeConfig(c);
  //
  // Although you could immediately (and atomically) add the new job to a
  // copy of config_ (so that we would immediately reply with the new job to
  // thrift queries), this is a bad idea, because this update would be
  // inconsistent e.g. with respect to disk writes by other schedulers.  In
  // other words, the only way to update config_ should be refresh -- a
  // clean load from the source of truth.
  throw BistroException("Not implemented for files");
}

}}
