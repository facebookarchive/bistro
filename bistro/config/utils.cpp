#include "bistro/bistro/config/utils.h"

namespace facebook { namespace bistro {

using namespace std;
using folly::dynamic;

void update(folly::dynamic& d, const folly::dynamic& d2) {
  for (const auto& pair : d2.items()) {
    d[pair.first] = pair.second;
  }
}

folly::dynamic merge(const folly::dynamic& d, const folly::dynamic& d2) {
  folly::dynamic ret(d);
  update(ret, d2);
  return ret;
}

}}
