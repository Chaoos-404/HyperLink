# Cross-Platform Peer Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable IPv4 local-subnet peer-discovery API that selects and connects to the peer with the highest measured TCP throughput on macOS, Linux, and Windows.

**Architecture:** `peer_discovery` owns protocol parsing, interface enumeration, UDP broadcast discovery, TCP probe ranking, and a responder. `file_transfer` consumes this API. Native sockets stay private; deterministic test seams cover parsing and ranking without a physical network.

**Tech Stack:** C++20, CMake, BSD sockets on macOS/Linux, Winsock and IP Helper API on Windows, CTest.

## Global Constraints

- Support macOS, Linux, and Windows without third-party runtime dependencies.
- Discover active local IPv4 subnets and loopback only; do not add IPv6, mDNS, Internet, or relays.
- Send `HLINK_DISCOVER_V2`; respond with `HLINK_PEER_V2 <transfer-port> <probe-port> <parallel-streams> <display-name>`.
- Select by measured TCP bytes per second; resolve ties by ascending IPv4 address then transfer port.
- Preserve direct `--host` behavior and existing transfer framing.
- Do not stage or modify the user's existing changes in `CMakeLists.txt`, `README.md`, or `.idea/`.

---

## File Structure

- Create `include/hyperlink/peer_discovery.hpp`: public types, discovery, selection, and responder API.
- Create `src/peer_discovery.cpp`: protocol parsing, native sockets, UDP discovery, TCP probe, and responder.
- Create `tests/peer_discovery_tests.cpp`: pure unit and loopback integration tests.
- Modify `CMakeLists.txt`: compile the source and test.
- Modify `tests/test_main.cpp`: invoke test functions.
- Modify `examples/file_transfer.cpp`: consume library discovery and advertise V2 peers.
- Modify `README.md`: document throughput ranking and ports.

### Task 1: Public API and deterministic discovery core

**Files:**
- Create `include/hyperlink/peer_discovery.hpp`
- Create `src/peer_discovery.cpp`
- Create `tests/peer_discovery_tests.cpp`
- Modify `tests/test_main.cpp`
- Modify `CMakeLists.txt`

**Interfaces:**
- Produces `PeerEndpoint { std::string host; std::uint16_t transfer_port; std::uint16_t probe_port; std::uint32_t parallel_streams; }`.
- Produces `DiscoveredPeer { PeerEndpoint endpoint; std::string display_name; std::string source_address; double measured_bytes_per_second; }`.
- Produces `PeerDiscoveryOptions` with default UDP port `47789`, discovery timeout `1500 ms`, TCP probe timeout `500 ms`, and payload `256 KiB`.
- Produces `PeerDiscovery::discover`, `PeerDiscovery::select_fastest`, and `PeerDiscoveryError`.

- [ ] **Step 1: Write the failing protocol and tie-break tests**

```cpp
void parses_v2_response_and_uses_source_address() {
  const auto peer = hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 47790 47791 8 receiver", "169.254.10.2");
  assert(peer && peer->endpoint.host == "169.254.10.2");
  assert(peer->endpoint.transfer_port == 47790 && peer->endpoint.probe_port == 47791);
}

void rejects_invalid_v2_response() {
  assert(!hyperlink::parse_peer_advertisement_for_testing(
      "HLINK_PEER_V2 0 47791 8 receiver", "192.168.1.2"));
}

void chooses_highest_rate_then_stable_tie_breaker() {
  const auto winner = hyperlink::select_fastest_peer_for_testing({
      {{"192.168.1.9", 47790, 47791, 1}, "nine", "192.168.1.9", 10.0},
      {{"192.168.1.4", 47790, 47791, 1}, "four", "192.168.1.4", 10.0},
      {{"192.168.1.3", 47790, 47791, 1}, "three", "192.168.1.3", 20.0}});
  assert(winner.endpoint.host == "192.168.1.3");
}
```

- [ ] **Step 2: Run the test and verify RED**

Run: `cmake --preset debug && cmake --build --preset debug && ./build/debug/hyperlink_tests`

Expected: compile failure because `peer_discovery.hpp` and its functions do not exist.

- [ ] **Step 3: Implement the smallest pure core**

```cpp
[[nodiscard]] std::optional<DiscoveredPeer> parse_peer_advertisement_for_testing(
    std::string_view message, std::string_view source_address);
[[nodiscard]] DiscoveredPeer select_fastest_peer_for_testing(
    std::vector<DiscoveredPeer> candidates);
```

Validate a 512-byte message limit, exact V2 prefix, ports in `1..65535`, and positive stream count. Source address supplies `endpoint.host`. Sort selection by descending rate, source IPv4, then transfer port. Add the source and test target to CMake and call all three tests from `tests/test_main.cpp`.

- [ ] **Step 4: Run the test and verify GREEN**

Run: `cmake --build --preset debug && ./build/debug/hyperlink_tests`

Expected: exit status `0`.

- [ ] **Step 5: Commit Task 1**

Run: `git add include/hyperlink/peer_discovery.hpp src/peer_discovery.cpp tests/peer_discovery_tests.cpp tests/test_main.cpp CMakeLists.txt && git commit -m "feat: add peer discovery protocol core"`

### Task 2: Native UDP discovery and bounded TCP probes

**Files:**
- Modify `include/hyperlink/peer_discovery.hpp`
- Modify `src/peer_discovery.cpp`
- Modify `tests/peer_discovery_tests.cpp`
- Modify `include/hyperlink/network_transport.hpp`

**Interfaces:**
- Consumes Task 1 types and parser.
- Produces `PeerDiscovery::discover(const PeerDiscoveryOptions&) -> std::vector<DiscoveredPeer>`.
- Produces `PeerDiscovery::select_fastest(const PeerDiscoveryOptions&) -> DiscoveredPeer`.
- Produces `PeerDiscovery::connect_fastest(const PeerDiscoveryOptions&, TcpEndpoint) -> std::unique_ptr<Transport>`.

- [ ] **Step 1: Write failing candidate/probe tests**

```cpp
void probes_every_deduplicated_candidate_and_chooses_fastest() {
  auto harness = hyperlink::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 slow", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47791 1 slow", "192.168.1.8"},
                     {"HLINK_PEER_V2 47790 47792 1 fast", "192.168.1.9"}};
  harness.probe_rates = {{"192.168.1.8", 5.0}, {"192.168.1.9", 15.0}};
  assert(harness.select_fastest().endpoint.host == "192.168.1.9");
  assert(harness.probed_hosts == std::vector<std::string>{"192.168.1.8", "192.168.1.9"});
}

void reports_all_failed_probes() {
  auto harness = hyperlink::PeerDiscoveryTestHarness{};
  harness.replies = {{"HLINK_PEER_V2 47790 47791 1 one", "192.168.1.8"}};
  harness.probe_failures = {{"192.168.1.8", "connection refused"}};
  try { static_cast<void>(harness.select_fastest()); assert(false); }
  catch (const hyperlink::PeerDiscoveryError& error) {
    assert(std::string{error.what()}.find("connection refused") != std::string::npos);
  }
}
```

- [ ] **Step 2: Run the test and verify RED**

Run: `cmake --build --preset debug && ./build/debug/hyperlink_tests`

Expected: compile failure because the harness and production discovery methods do not exist.

- [ ] **Step 3: Implement platform discovery and probing**

Use an internal injectable interface provider and probe runner. On macOS/Linux enumerate active IPv4 interfaces with `getifaddrs`, `IFF_UP`, `ifa_broadaddr`, and netmask fallback. On Windows use `GetAdaptersAddresses(AF_INET)`, `IfOperStatusUp`, and `OnLinkPrefixLength`. Add directed broadcast, `255.255.255.255`, and loopback targets; remove duplicate `(bind-address, destination)` pairs.

Bind a UDP socket per target, send `HLINK_DISCOVER_V2`, collect unicast V2 replies until the deadline, and deduplicate by host plus transfer/probe ports. Probe every candidate using a TCP receive connection bounded by `probe_timeout`; calculate bytes divided by `steady_clock` elapsed time. Continue after target or probe failures. Throw `PeerDiscoveryError("no Hyperlink peer discovered")` for no replies and include every host failure if all probes fail. `connect_fastest` must retain caller buffer settings while replacing only host and transfer port.

- [ ] **Step 4: Run the test and verify GREEN**

Run: `cmake --build --preset debug && ./build/debug/hyperlink_tests`

Expected: exit status `0`; test evidence shows both candidates were probed and all-failure diagnostics include the socket reason.

- [ ] **Step 5: Commit Task 2**

Run: `git add include/hyperlink/peer_discovery.hpp src/peer_discovery.cpp include/hyperlink/network_transport.hpp tests/peer_discovery_tests.cpp && git commit -m "feat: discover and rank local TCP peers"`

### Task 3: Responder, CLI integration, and documentation

**Files:**
- Modify `include/hyperlink/peer_discovery.hpp`
- Modify `src/peer_discovery.cpp`
- Modify `examples/file_transfer.cpp`
- Modify `tests/peer_discovery_tests.cpp`
- Modify `README.md`

**Interfaces:**
- Produces `PeerDiscoveryResponder` with RAII `start()` and `stop()`.
- Consumes `PeerDiscovery::select_fastest` for `hyperlink_file --send --auto`.

- [ ] **Step 1: Write the failing loopback integration test**

```cpp
void loopback_responder_is_discovered_and_has_positive_throughput() {
  auto responder = hyperlink::PeerDiscoveryResponder{
      {.bind_host = "127.0.0.1", .transfer_port = 47790, .probe_port = 47791,
       .discovery_port = 47789, .display_name = "loopback"}};
  responder.start();
  const auto peer = hyperlink::PeerDiscovery{}.select_fastest(
      {.discovery_port = 47789, .discovery_timeout = std::chrono::milliseconds{500},
       .probe_timeout = std::chrono::milliseconds{250}});
  responder.stop();
  assert(peer.endpoint.host == "127.0.0.1" && peer.measured_bytes_per_second > 0.0);
}
```

- [ ] **Step 2: Run the test and verify RED**

Run: `cmake --build --preset debug && ./build/debug/hyperlink_tests`

Expected: compile failure because the responder API does not exist.

- [ ] **Step 3: Implement the responder and CLI migration**

Implement `PeerDiscoveryResponder` with a UDP listener that replies exactly in V2 format and a TCP probe listener that writes a reusable byte buffer until client close or its deadline. Start it for `--receive` unless `--no-advertise` is passed.

In `examples/file_transfer.cpp`, remove embedded V1 discovery and `DiscoveryResponder`. For `--send --auto`, select a peer then assign its host, transfer port, and parallel count before opening transfer streams. Remove `--allow-lan` parsing and usage because all local subnets are in scope. Do not change direct `--host` operation. Update README with TCP-probe ranking, the discovery/probe ports, and Windows firewall rules for UDP `47789` plus the TCP probe port.

- [ ] **Step 4: Run integration and full verification**

Run: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure && ./build/debug/hyperlink_file --help`

Expected: all CTest cases pass; help lists `--auto` and no longer lists `--allow-lan`.

- [ ] **Step 5: Commit Task 3**

Run: `git add include/hyperlink/peer_discovery.hpp src/peer_discovery.cpp examples/file_transfer.cpp tests/peer_discovery_tests.cpp README.md && git commit -m "feat: use throughput-ranked peer discovery for transfers"`

## Final Verification

- [ ] Run `git diff --check`.
- [ ] Run `cmake --preset debug && cmake --build --preset debug && ctest --preset debug --output-on-failure`.
- [ ] Confirm `git status --short` still reports the user's existing `CMakeLists.txt`, `README.md`, and `.idea/` changes separately from feature work.
- [ ] Build and run CTest on macOS, Linux, and Windows CI runners; verify loopback discovery on each platform.
