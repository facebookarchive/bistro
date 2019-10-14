/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/hostname.h"

#include <glog/logging.h>
#include <iomanip>
#include <limits.h>
#include <netdb.h>
#include <sys/socket.h>
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


  if (auto err = getaddrinfo(hostname, nullptr, &hint, &info)) {
    if (err == EAI_SYSTEM) {
      PLOG(ERROR) << "System error qualifying qualify local hostname";
    } else {
      LOG(ERROR) << "Error qualifying qualify local hostname: "
        << gai_strerror(err);
    }
    return "";
  }
  const string canon_name = info->ai_canonname;
  freeaddrinfo(info);

  return canon_name;
}

}}
