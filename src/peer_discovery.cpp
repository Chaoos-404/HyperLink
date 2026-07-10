#include "hyperlink/peer_discovery.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <tuple>

namespace hyperlink {

namespace {

constexpr std::size_t kMaximumAdvertisementBytes = 512;
constexpr std::string_view kAdvertisementPrefix = "HLINK_PEER_V2 ";

[[nodiscard]] bool is_decimal(std::string_view value) {
  return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
    return std::isdigit(character) != 0;
  });
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_decimal(std::string_view value) {
  if (!is_decimal(value)) {
    return std::nullopt;
  }

  Integer parsed{};
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

[[nodiscard]] std::optional<std::array<unsigned int, 4>> parse_ipv4(std::string_view address) {
  std::array<unsigned int, 4> octets{};
  std::size_t position = 0;

  for (std::size_t index = 0; index < octets.size(); ++index) {
    const auto separator = address.find('.', position);
    if ((index + 1 == octets.size()) != (separator == std::string_view::npos)) {
      return std::nullopt;
    }
    const auto part = address.substr(position, separator - position);
    const auto parsed = parse_decimal<unsigned int>(part);
    if (!parsed || *parsed > 255) {
      return std::nullopt;
    }
    octets[index] = *parsed;

    if (index + 1 == octets.size()) {
      position = address.size();
    } else {
      position = separator + 1;
    }
  }

  return position == address.size() ? std::optional{octets} : std::nullopt;
}

[[nodiscard]] bool source_address_precedes(const DiscoveredPeer& left,
                                            const DiscoveredPeer& right) {
  const auto left_address = parse_ipv4(left.source_address);
  const auto right_address = parse_ipv4(right.source_address);
  if (left_address && right_address && *left_address != *right_address) {
    return *left_address < *right_address;
  }
  if (left.source_address != right.source_address) {
    return left.source_address < right.source_address;
  }
  return left.endpoint.transfer_port < right.endpoint.transfer_port;
}

} // namespace

std::optional<DiscoveredPeer> parse_peer_advertisement_for_testing(std::string_view message,
                                                                    std::string_view source_address) {
  if (message.size() > kMaximumAdvertisementBytes || !message.starts_with(kAdvertisementPrefix) ||
      !parse_ipv4(source_address)) {
    return std::nullopt;
  }

  const auto fields = message.substr(kAdvertisementPrefix.size());
  const auto transfer_end = fields.find(' ');
  if (transfer_end == std::string_view::npos) {
    return std::nullopt;
  }
  const auto probe_end = fields.find(' ', transfer_end + 1);
  if (probe_end == std::string_view::npos) {
    return std::nullopt;
  }
  const auto streams_end = fields.find(' ', probe_end + 1);
  if (streams_end == std::string_view::npos || streams_end + 1 == fields.size()) {
    return std::nullopt;
  }

  const auto transfer_port = parse_decimal<unsigned int>(fields.substr(0, transfer_end));
  const auto probe_port = parse_decimal<unsigned int>(
      fields.substr(transfer_end + 1, probe_end - transfer_end - 1));
  const auto parallel_streams = parse_decimal<std::uint32_t>(
      fields.substr(probe_end + 1, streams_end - probe_end - 1));
  if (!transfer_port || !probe_port || !parallel_streams || *transfer_port == 0 ||
      *transfer_port > 65535 || *probe_port == 0 || *probe_port > 65535 ||
      *parallel_streams == 0) {
    return std::nullopt;
  }

  return DiscoveredPeer{
      .endpoint = PeerEndpoint{
          .host = std::string{source_address},
          .transfer_port = static_cast<std::uint16_t>(*transfer_port),
          .probe_port = static_cast<std::uint16_t>(*probe_port),
          .parallel_streams = *parallel_streams,
      },
      .display_name = std::string{fields.substr(streams_end + 1)},
      .source_address = std::string{source_address},
  };
}

DiscoveredPeer select_fastest_peer_for_testing(std::vector<DiscoveredPeer> candidates) {
  if (candidates.empty()) {
    throw std::invalid_argument("peer candidates must not be empty");
  }

  std::ranges::sort(candidates, [](const DiscoveredPeer& left, const DiscoveredPeer& right) {
    if (left.measured_bytes_per_second != right.measured_bytes_per_second) {
      return left.measured_bytes_per_second > right.measured_bytes_per_second;
    }
    return source_address_precedes(left, right);
  });
  return candidates.front();
}

} // namespace hyperlink
