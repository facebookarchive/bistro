/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/SettingsMap.h"

#include <folly/dynamic.h>

namespace facebook { namespace bistro {

using namespace folly;
using namespace std;

SettingsMap::SettingsMap(const dynamic& d) {
  if (!d.isObject()) {
    throw BistroException("Can only create settings map from an object");
  }
  for (const auto& pair : d.items()) {
    const string& name = pair.first.asString().toStdString();
    set(name, pair.second);
  }
}

}}
