#pragma once

#include "hyperlink/export.hpp"
#include "hyperlink/network_transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hyperlink {

struct HYPERLINK_API PeerEndpoint {
  std::string host;
  std::uint16_t transfer_port{0};
  std::uint16_t probe_port{0};
  std::uint32_t parallel_streams{0};
};

struct HYPERLINK_API DiscoveredPeer {
  PeerEndpoint endpoint;
  std::string display_name;
  std::string source_address;
  double measured_bytes_per_second{0.0};
};

struct HYPERLINK_API PeerDiscoveryOptions {
  std::uint16_t discovery_port{47789};
  std::chrono::milliseconds discovery_timeout{1500};
  std::chrono::milliseconds probe_timeout{500};
  std::uint32_t probe_payload_bytes{256 * 1024};
};

class HYPERLINK_API PeerDiscoveryError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class HYPERLINK_API PeerDiscovery {
public:
  [[nodiscard]] std::vector<DiscoveredPeer> discover(const PeerDiscoveryOptions& options);
  [[nodiscard]] DiscoveredPeer select_fastest(const PeerDiscoveryOptions& options);
  [[nodiscard]] std::unique_ptr<Transport> connect_fastest(const PeerDiscoveryOptions& options,
                                                            TcpEndpoint endpoint);
};

[[nodiscard]] HYPERLINK_API std::optional<DiscoveredPeer> parse_peer_advertisement_for_testing(
    std::string_view message, std::string_view source_address);
[[nodiscard]] HYPERLINK_API DiscoveredPeer
select_fastest_peer_for_testing(std::vector<DiscoveredPeer> candidates);

} // namespace hyperlink
