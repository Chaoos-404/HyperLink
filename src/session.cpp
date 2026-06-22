#include "hyperlink/session.hpp"

#include <stdexcept>
#include <utility>

namespace hyperlink {

Session::Session(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {
  if (!transport_) {
    throw std::invalid_argument("Session requires a transport");
  }
}

Session::~Session() { close(); }

Session::Session(Session&& other) noexcept = default;

auto Session::operator=(Session&& other) noexcept -> Session& {
  if (this != &other) {
    close();
    transport_ = std::move(other.transport_);
    connected_ = other.connected_;
    other.connected_ = false;
  }
  return *this;
}

std::vector<PeerInfo> Session::discover(const AutoConfigOptions& options) {
  if (const auto error = validate(options)) {
    throw std::invalid_argument(*error);
  }
  return transport_->discover(options);
}

void Session::connect(const PeerInfo& peer, const AutoConfigOptions& local_options,
                      const AutoConfigOptions& peer_options, std::string session_name) {
  const auto config = negotiate(local_options, peer_options, std::move(session_name));
  transport_->connect(peer, config);
  connected_ = true;
}

std::size_t Session::send(std::span<const std::byte> payload) {
  if (!connected_) {
    throw std::logic_error("Session is not connected");
  }
  return transport_->send(payload);
}

std::vector<std::byte> Session::receive(std::size_t max_bytes) {
  if (!connected_) {
    throw std::logic_error("Session is not connected");
  }
  return transport_->receive(max_bytes);
}

std::size_t Session::receive_into(std::span<std::byte> buffer) {
  if (!connected_) {
    throw std::logic_error("Session is not connected");
  }
  return transport_->receive_into(buffer);
}

void Session::close() noexcept {
  if (transport_ && connected_) {
    transport_->close();
    connected_ = false;
  }
}

std::string to_string(UsbSpeed speed) {
  switch (speed) {
  case UsbSpeed::Unknown:
    return "unknown";
  case UsbSpeed::LowSpeed:
    return "USB low-speed";
  case UsbSpeed::FullSpeed:
    return "USB full-speed";
  case UsbSpeed::HighSpeed:
    return "USB high-speed";
  case UsbSpeed::SuperSpeed:
    return "USB 5Gbps";
  case UsbSpeed::SuperSpeedPlus:
    return "USB 10Gbps/20Gbps";
  case UsbSpeed::Usb4_20Gbps:
    return "USB4 20Gbps";
  case UsbSpeed::Usb4_40Gbps:
    return "USB4 40Gbps";
  case UsbSpeed::Usb4_80Gbps:
    return "USB4/USB 80Gbps";
  }
  return "unknown";
}

} // namespace hyperlink
