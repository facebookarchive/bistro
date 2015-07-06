#pragma once

#include "folly/dynamic.h"

namespace facebook { namespace bistro {

void update(folly::dynamic&, const folly::dynamic&);

folly::dynamic merge(const folly::dynamic&, const folly::dynamic&);

}}
