#include "hyperlink/auto_config.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hyperlink {

namespace {

constexpr std::uint32_t kMinPacketBytes = 1024;
constexpr std::uint32_t kMaxPacketBytes = 1024 * 1024;

[[nodiscard]] int profile_rank(TransferProfile profile) {
  switch (profile) {
  case TransferProfile::LowLatency:
    return 0;
  case TransferProfile::Balanced:
    return 1;
  case TransferProfile::Throughput:
    return 2;
  }
  return 1;
}

[[nodiscard]] TransferProfile profile_from_rank(int rank) {
  switch (rank) {
  case 0:
    return TransferProfile::LowLatency;
  case 2:
    return TransferProfile::Throughput;
  case 1:
  default:
    return TransferProfile::Balanced;
  }
}

} // namespace

std::optional<std::string> validate(const AutoConfigOptions& options) {
  if (options.preferred_packet_bytes < kMinPacketBytes) {
    return "preferred_packet_bytes must be at least 1024 bytes";
  }

  if (options.preferred_packet_bytes > kMaxPacketBytes) {
    return "preferred_packet_bytes must be no larger than 1048576 bytes";
  }

  if (options.discovery_timeout.count() <= 0) {
    return "discovery_timeout must be positive";
  }

  return std::nullopt;
}

NegotiatedConfig negotiate(const AutoConfigOptions& local, const AutoConfigOptions& peer,
                           std::string session_name) {
  if (const auto error = validate(local)) {
    throw std::invalid_argument("local auto config is invalid: " + *error);
  }

  if (const auto error = validate(peer)) {
    throw std::invalid_argument("peer auto config is invalid: " + *error);
  }

  const auto packet_bytes = std::min(local.preferred_packet_bytes, peer.preferred_packet_bytes);
  const auto profile =
      profile_from_rank(std::min(profile_rank(local.profile), profile_rank(peer.profile)));

  auto mode = local.mode;
  if (local.mode == LinkMode::Auto) {
    mode = peer.mode == LinkMode::Host ? LinkMode::Device : LinkMode::Host;
  }

  return NegotiatedConfig{
      .local_mode = mode,
      .profile = profile,
      .packet_bytes = packet_bytes,
      .encryption_enabled = local.require_encryption || peer.require_encryption,
      .session_name = std::move(session_name),
  };
}

std::string to_string(LinkMode mode) {
  switch (mode) {
  case LinkMode::Host:
    return "host";
  case LinkMode::Device:
    return "device";
  case LinkMode::Auto:
    return "auto";
  }
  return "unknown";
}

std::string to_string(TransferProfile profile) {
  switch (profile) {
  case TransferProfile::LowLatency:
    return "low-latency";
  case TransferProfile::Balanced:
    return "balanced";
  case TransferProfile::Throughput:
    return "throughput";
  }
  return "unknown";
}

} // namespace hyperlink
