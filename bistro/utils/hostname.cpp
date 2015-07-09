/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "bistro/bistro/utils/hostname.h"

#include <arpa/inet.h>
#include <glog/logging.h>
#include <iomanip>
#include <limits.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace facebook { namespace bistro {

using namespace std;

string getLocalHostName() {
  char hostname[HOST_NAME_MAX];

  if (0 != gethostname(hostname, HOST_NAME_MAX)) {
    PLOG(ERROR) << "Could not get local hostname";
    return "";
  }
  hostname[HOST_NAME_MAX - 1] = '\0';  // gethostname may not null-terminate

  // Resolve to a FQDN, since the local hostname isn't guaranteed to be.
  struct addrinfo* info, hint;

  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_UNSPEC;
  hint.ai_flags = AI_CANONNAME;

  if (0 != getaddrinfo(hostname, nullptr, &hint, &info)) {
    PLOG(ERROR) << "Failed to fully qualify local hostname";
    return "";
  }
  const string canon_name = info->ai_canonname;
  freeaddrinfo(info);

  return canon_name;
}

}}
