#include "hyperlink/auto_config.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>

void validation_rejects_bad_packet_size() {
  auto options = hyperlink::AutoConfigOptions{};
  options.preferred_packet_bytes = 512;

  const auto error = hyperlink::validate(options);
  assert(error.has_value());
}

void negotiation_chooses_conservative_shared_settings() {
  auto local = hyperlink::AutoConfigOptions{};
  local.mode = hyperlink::LinkMode::Auto;
  local.profile = hyperlink::TransferProfile::Throughput;
  local.preferred_packet_bytes = 256 * 1024;
  local.require_encryption = false;

  auto peer = hyperlink::AutoConfigOptions{};
  peer.mode = hyperlink::LinkMode::Host;
  peer.profile = hyperlink::TransferProfile::Balanced;
  peer.preferred_packet_bytes = 64 * 1024;
  peer.require_encryption = true;

  const auto config = hyperlink::negotiate(local, peer, "bench");

  assert(config.local_mode == hyperlink::LinkMode::Device);
  assert(config.profile == hyperlink::TransferProfile::Balanced);
  assert(config.packet_bytes == 64 * 1024);
  assert(config.encryption_enabled);
  assert(config.session_name == "bench");
}

void negotiation_throws_on_invalid_input() {
  auto local = hyperlink::AutoConfigOptions{};
  local.discovery_timeout = std::chrono::milliseconds{0};

  bool threw = false;
  try {
    static_cast<void>(hyperlink::negotiate(local, hyperlink::AutoConfigOptions{}, "bad"));
  } catch (const std::invalid_argument&) {
    threw = true;
  }

  assert(threw);
}
