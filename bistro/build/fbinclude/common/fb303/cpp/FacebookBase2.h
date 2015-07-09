/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
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
