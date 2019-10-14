/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    const string& name = pair.first.asString();
    set(name, pair.second);
  }
}

}}
