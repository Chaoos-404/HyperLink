#include "hyperlink/peer_discovery.hpp"

#include <cassert>
#include <map>
#include <string>
#include <vector>

void parses_v2_response_and_uses_source_address() {
  const auto peer = hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 47790 47791 8 receiver", "169.254.10.2");

  assert(peer && peer->endpoint.host == "169.254.10.2");
  assert(peer->endpoint.transfer_port == 47790 && peer->endpoint.probe_port == 47791);
}

void rejects_invalid_v2_response() {
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 0 47791 8 receiver", "192.168.1.2"));
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V1 47790 47791 8 receiver", "192.168.1.2"));
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 47790 47791 0 receiver", "192.168.1.2"));
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      std::string(513, 'a'), "192.168.1.2"));
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 47790 47791 8 receiver", "not-an-ipv4-address"));
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 47790 47791 8 receiver", "1.2.3.4."));
}

void chooses_highest_rate_then_stable_tie_breaker() {
  const auto winner = hyperlink::select_fastest_peer_for_testing({
      {{"192.168.1.9", 47790, 47791, 1}, "nine", "192.168.1.9", 10.0},
      {{"192.168.1.4", 47790, 47791, 1}, "four", "192.168.1.4", 10.0},
      {{"192.168.1.3", 47790, 47791, 1}, "three", "192.168.1.3", 20.0}});

  assert(winner.endpoint.host == "192.168.1.3");

  const auto tie_winner = hyperlink::select_fastest_peer_for_testing({
      {{"192.168.1.9", 47790, 47791, 1}, "nine", "192.168.1.9", 10.0},
      {{"192.168.1.4", 47791, 47792, 1}, "four-high-port", "192.168.1.4", 10.0},
      {{"192.168.1.4", 47790, 47791, 1}, "four-low-port", "192.168.1.4", 10.0}});
  assert(tie_winner.endpoint.host == "192.168.1.4");
  assert(tie_winner.endpoint.transfer_port == 47790);
}

void probes_every_deduplicated_candidate_and_chooses_fastest() {
  auto harness = hyperlink::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 slow", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47791 1 slow", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47792 1 fast", "192.168.1.9"}};
  harness.probe_rates = {{"192.168.1.8", 5.0}, {"192.168.1.9", 15.0}};

  assert(harness.select_fastest().endpoint.host == "192.168.1.9");
  assert((harness.probed_hosts == std::vector<std::string>{"192.168.1.8", "192.168.1.9"}));
}

void reports_all_failed_probes() {
  auto harness = hyperlink::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 one", "192.168.1.8"}};
  harness.probe_failures = {{"192.168.1.8", "connection refused"}};

  try {
    static_cast<void>(harness.select_fastest());
    assert(false);
  } catch (const hyperlink::PeerDiscoveryError& error) {
    assert(std::string{error.what()}.find("connection refused") != std::string::npos);
  }
}
