#include "hyperlink/session.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <vector>

class FakeTransport final : public hyperlink::Transport {
public:
  [[nodiscard]] std::vector<hyperlink::PeerInfo>
  discover(const hyperlink::AutoConfigOptions& /*options*/) override {
    return {hyperlink::PeerInfo{
        .id = "peer-1",
        .display_name = "Test Peer",
        .supports_high_speed = true,
        .supports_super_speed = true,
    }};
  }

  void connect(const hyperlink::PeerInfo& peer,
               const hyperlink::NegotiatedConfig& config) override {
    connected = true;
    connected_peer = peer.id;
    packet_bytes = config.packet_bytes;
  }

  [[nodiscard]] std::size_t send(std::span<const std::byte> payload) override {
    sent_bytes += payload.size();
    return payload.size();
  }

  [[nodiscard]] std::vector<std::byte> receive(std::size_t max_bytes) override {
    return std::vector<std::byte>(max_bytes, std::byte{0x2A});
  }

  [[nodiscard]] std::size_t receive_into(std::span<std::byte> buffer) override {
    std::fill(buffer.begin(), buffer.end(), std::byte{0x2A});
    return buffer.size();
  }

  void close() noexcept override { connected = false; }

  bool connected{false};
  std::string connected_peer;
  std::uint32_t packet_bytes{0};
  std::size_t sent_bytes{0};
};

void session_requires_transport() {
  bool threw = false;
  try {
    auto session = hyperlink::Session{nullptr};
  } catch (const std::invalid_argument&) {
    threw = true;
  }

  assert(threw);
}

void session_discovers_and_transfers_after_connect() {
  auto transport = std::make_unique<FakeTransport>();
  auto* raw_transport = transport.get();
  auto session = hyperlink::Session{std::move(transport)};

  const auto peers = session.discover(hyperlink::AutoConfigOptions{});
  assert(peers.size() == 1);

  session.connect(peers.front(), hyperlink::AutoConfigOptions{}, hyperlink::AutoConfigOptions{},
                  "session-test");
  assert(raw_transport->connected);
  assert(raw_transport->connected_peer == "peer-1");

  const auto payload = std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}};
  assert(session.send(payload) == payload.size());

  const auto received = session.receive(3);
  assert(received.size() == 3);

  session.close();
  assert(!raw_transport->connected);
}

void send_requires_connected_session() {
  auto session = hyperlink::Session{std::make_unique<FakeTransport>()};
  bool threw = false;

  try {
    const auto payload = std::vector<std::byte>{std::byte{0x01}};
    static_cast<void>(session.send(payload));
  } catch (const std::logic_error&) {
    threw = true;
  }

  assert(threw);
}
