#include "../src/peer_discovery_internal.hpp"

#include <cassert>
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
      {{"192.168.1.4", 47790, 47792, 1}, "four-high-probe", "192.168.1.4", 10.0},
      {{"192.168.1.4", 47790, 47791, 1}, "four-low-probe", "192.168.1.4", 10.0}});
  assert(tie_winner.endpoint.host == "192.168.1.4");
  assert(tie_winner.endpoint.transfer_port == 47790);
  assert(tie_winner.endpoint.probe_port == 47791);
}

void probes_every_deduplicated_candidate_and_chooses_fastest() {
  auto harness = hyperlink::detail::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 slow", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47791 1 slow", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47792 1 fast", "192.168.1.9"}};
  harness.probe_rates = {{{"192.168.1.8", 47791}, 5.0}, {{"192.168.1.9", 47792}, 15.0}};

  assert(harness.select_fastest().endpoint.host == "192.168.1.9");
  assert((harness.probed_endpoints == std::vector<hyperlink::detail::ProbeEndpointKey>{
      {"192.168.1.8", 47791}, {"192.168.1.9", 47792}}));
}

void reports_all_failed_probes() {
  auto harness = hyperlink::detail::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 one", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47792 1 two", "192.168.1.9"}};
  harness.probe_failures = {{{"192.168.1.8", 47791}, "connection refused"},
                            {{"192.168.1.9", 47792}, "probe timed out"}};

  try {
    static_cast<void>(harness.select_fastest());
    assert(false);
  } catch (const hyperlink::PeerDiscoveryError& error) {
    assert(std::string{error.what()}.find("connection refused") != std::string::npos);
    assert(std::string{error.what()}.find("probe timed out") != std::string::npos);
  }
}

void probes_same_host_at_each_distinct_probe_port() {
  auto harness = hyperlink::detail::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47793 1 slower", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47792 1 faster", "192.168.1.8"}};
  harness.probe_rates = {{{"192.168.1.8", 47793}, 5.0}, {{"192.168.1.8", 47792}, 15.0}};

  const auto peer = harness.select_fastest();
  assert(peer.endpoint.probe_port == 47792);
  assert((harness.probed_endpoints == std::vector<hyperlink::detail::ProbeEndpointKey>{
      {"192.168.1.8", 47793}, {"192.168.1.8", 47792}}));
}

void continues_after_a_timed_out_probe() {
  auto harness = hyperlink::detail::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 timeout", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47792 1 available", "192.168.1.9"}};
  harness.probe_failures = {{{"192.168.1.8", 47791}, "probe timed out"}};
  harness.probe_rates = {{{"192.168.1.9", 47792}, 15.0}};

  assert(harness.select_fastest().endpoint.host == "192.168.1.9");
  assert((harness.probed_endpoints == std::vector<hyperlink::detail::ProbeEndpointKey>{
      {"192.168.1.8", 47791}, {"192.168.1.9", 47792}}));
}

void selected_connection_retains_caller_buffer_settings() {
  const auto endpoint = hyperlink::detail::selected_tcp_endpoint_for_testing(
      {.host = "manual-host", .port = 49999, .send_buffer_bytes = 4096, .receive_buffer_bytes = 8192},
      {{"192.168.1.8", 47790, 47791, 1}, "receiver", "192.168.1.8", 1.0});

  assert(endpoint.host == "192.168.1.8" && endpoint.port == 47790);
  assert(endpoint.send_buffer_bytes == 4096 && endpoint.receive_buffer_bytes == 8192);
}
