#pragma once

#include "hyperlink/peer_discovery.hpp"

#include <compare>
#include <map>

namespace hyperlink::detail {

struct ProbeEndpointKey {
  std::string host;
  std::uint16_t probe_port;

  auto operator<=>(const ProbeEndpointKey&) const = default;
};

struct PeerDiscoveryTestHarness {
  std::vector<std::pair<std::string, std::string>> replies;
  std::map<ProbeEndpointKey, double> probe_rates;
  std::map<ProbeEndpointKey, std::string> probe_failures;
  std::vector<ProbeEndpointKey> probed_endpoints;

  [[nodiscard]] DiscoveredPeer select_fastest();
};

[[nodiscard]] TcpEndpoint
selected_tcp_endpoint_for_testing(TcpEndpoint endpoint, const DiscoveredPeer& peer);

[[nodiscard]] bool discovery_datagram_within_limit_for_testing(std::size_t received_bytes);

} // namespace hyperlink::detail
