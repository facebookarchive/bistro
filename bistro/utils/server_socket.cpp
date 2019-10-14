/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "bistro/bistro/utils/server_socket.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <iomanip>
#include <net/if.h>
#include <netdb.h>

// NOTE(facebook): Parts are copy-pasta'd from common/network/ for open-source.

DEFINE_string(
  server_inet_interface, "",
  "If --server_address is not specified, use this network interface to find "
  "an address to listen to (e.g.  eth0).  Defaults to empty, meaning: try "
  "to guess the primary external interface.  Unless --server_disable_ipv6 "
  "is specified, tries IPv6 first, then IPv4."
);
DEFINE_bool(server_disable_ipv6, false, "See --server_inet_interface.");
DEFINE_string(
  server_address, "",
  "Listen on this IPv4 or IPv6 address. Defaults to empty, meaning: try to "
  "guess the interface and the address. See also --server_inet_interface."
);
DEFINE_int32(
  server_port, 0,
  "Listen on this port. Defaults to zero, meaning: pick a random free port."
);

namespace facebook { namespace bistro {

namespace {

bool isLoopback(struct ifaddrs* ifa) {
  return ifa->ifa_flags & IFF_LOOPBACK;
}

bool isLinkLocal(struct ifaddrs* ifa) {
  auto* sa6 = (struct sockaddr_in6*)ifa->ifa_addr;
  return ifa->ifa_addr->sa_family == AF_INET6
    && IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr);
}

struct ifaddrs* guessPrimaryInterface(struct ifaddrs* if_addrs, uint16_t af) {
  // Look for a specific interface if requested (may even be link-local...)
  if (!FLAGS_server_inet_interface.empty()) {
    for (auto* ifa = if_addrs; ifa; ifa = ifa->ifa_next) {
      if (
        ifa->ifa_addr &&
        ifa->ifa_addr->sa_family == af &&
        FLAGS_server_inet_interface == ifa->ifa_name
      ) {
        return ifa;
      }
    }
    LOG(INFO) << "Could not find " << af << " version of "
      << "--server_inet_interface " << FLAGS_server_inet_interface;
    return nullptr;
  }
  // look for an interface that starts with "eth"
  for (auto* ifa = if_addrs; ifa; ifa = ifa->ifa_next) {
    if (
      ifa->ifa_addr &&
      ifa->ifa_addr->sa_family == af &&
      !isLinkLocal(ifa) &&
      strncmp(ifa->ifa_name, "eth", 3) == 0
    ) {
      return ifa;
    }
  }
  // just get the first interface that is not a loopback
  for (auto* ifa = if_addrs; ifa; ifa = ifa->ifa_next) {
    if (
      ifa->ifa_addr &&
      ifa->ifa_addr->sa_family == af &&
      !isLoopback(ifa) &&
      !isLinkLocal(ifa)
    ) {
      return ifa;
    }
  }
  LOG(WARNING) << "Found no " << af << " interfaces that are not link-local "
    << "or loopback";
  return nullptr;
}

// Return the address in fe80:0000:0000:0000:0000:0000:0000:0001 format.
std::string ipv6AddrToString(const struct in6_addr& addr) {
  std::stringstream ipv6_out;
  if (IN6_IS_ADDR_V4MAPPED(&addr)) {
    char ip_str[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, (char*)&addr + 12, ip_str, INET_ADDRSTRLEN)) {
      LOG(ERROR) << "Failed to convert IPv6-mapped IPv4 address to string";
      return "";
    }
    ipv6_out << "0000:0000:0000:0000:0000:ffff:" << ip_str;
  } else {
    ipv6_out << std::hex << setiosflags(std::ios_base::right);
    for (int i = 0; i < 7; i++) {
      ipv6_out << std::setfill('0') << std::setw(4)
        << ntohs(addr.s6_addr16[i]) << ":";
    }
    ipv6_out << std::setfill('0') << std::setw(4)
      << ntohs(addr.s6_addr16[7]);
  }
  return ipv6_out.str();
}

/**
 * Finds the IPv6 (unless --server_disable_ipv6=false) or IPv4 address of
 * the network interface named by --server_inet_interface, or of our best
 * guess at the primary network interface if that flag is empty.
 *
 * Returns "" on failure.
 */
std::string guessPrimaryInterfaceAddress() {
  struct ifaddrs* if_addrs = nullptr;
  if (getifaddrs(&if_addrs) == -1) {
    PLOG(ERROR) << "getifaddrs() failed";
    return "";
  }

  SCOPE_EXIT {
    if (if_addrs) {
      freeifaddrs(if_addrs);
    }
  };

  // Look for IPv6 addresses first
  if (!FLAGS_server_disable_ipv6) {
    std::string addr_str;
    struct ifaddrs* ifa = guessPrimaryInterface(if_addrs, AF_INET6);
    if (ifa && ifa->ifa_addr) {
      addr_str =
        ipv6AddrToString(((struct sockaddr_in6*)ifa->ifa_addr)->sin6_addr);
      if (!addr_str.empty()) {
        return addr_str;
      }
      // Try IPv4 now; don't log since ipv6AddrToString logs already
    }
  }

  // Fall back to IPv4 addresses
  char addr_str[INET_ADDRSTRLEN] = { '\0' } ;
  struct ifaddrs* ifa = guessPrimaryInterface(if_addrs, AF_INET);
  if (ifa && ifa->ifa_addr) {
    auto addr_ptr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
    if (inet_ntop(AF_INET, addr_ptr, addr_str, INET_ADDRSTRLEN)) {
      return addr_str;
    }
    PLOG(ERROR) << "Failed to convert IPv4 address to string";
  }
  return "";
}

}  // anonymous namespace

std::pair<folly::AsyncServerSocket::UniquePtr, cpp2::ServiceAddress>
    getServerSocketAndAddress() {

  // Pick the address to listen on
  cpp2::ServiceAddress addr;
  if (FLAGS_server_address.empty()) {
    addr.ip_or_host = guessPrimaryInterfaceAddress();
    CHECK(!addr.ip_or_host.empty()) << "Could not find an appropriate "
      << "network interface. Try specifying --server_inet_interface or "
      << "--server_address.";
  } else {
    addr.ip_or_host = FLAGS_server_address;
  }

  // Pick the port
  folly::AsyncServerSocket::UniquePtr socket(new folly::AsyncServerSocket);
  if (FLAGS_server_port == 0) {
    socket->bind(folly::SocketAddress(addr.ip_or_host, 0));
    folly::SocketAddress address;
    socket->getAddress(&address);
    addr.port = address.getPort();
  } else {
    addr.port = FLAGS_server_port;
    socket->bind(folly::SocketAddress(addr.ip_or_host, addr.port));
  }

  return std::make_pair(std::move(socket), std::move(addr));
}

}}
