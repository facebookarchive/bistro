/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
// Stub for a service-monitoring interface used by Facebook infrastructure.

#include <string>

namespace facebook { namespace fb303 {
class FacebookBase2 : virtual public cpp2::FacebookServiceSvIf {
public:
  explicit FacebookBase2(std::string name) {}
  virtual fb303::cpp2::fb_status getStatus() {
    return fb303::cpp2::fb_status::ALIVE;
  }
  virtual fb303::cpp2::fb_status sync_getStatus() {
    return fb303::cpp2::fb_status::ALIVE;
  }
};
}}
