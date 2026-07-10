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

struct HYPERLINK_API PeerDiscoveryTestHarness {
  std::vector<std::pair<std::string, std::string>> replies;
  std::map<ProbeEndpointKey, double> probe_rates;
  std::map<ProbeEndpointKey, std::string> probe_failures;
  std::vector<ProbeEndpointKey> probed_endpoints;

  [[nodiscard]] DiscoveredPeer select_fastest();
};

[[nodiscard]] HYPERLINK_API TcpEndpoint
selected_tcp_endpoint_for_testing(TcpEndpoint endpoint, const DiscoveredPeer& peer);

} // namespace hyperlink::detail
