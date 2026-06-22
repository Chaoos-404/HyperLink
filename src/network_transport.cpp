#include "hyperlink/network_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace hyperlink {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

class WinsockRuntime {
public:
  WinsockRuntime() {
    auto data = WSADATA{};
    const auto result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }

  ~WinsockRuntime() { WSACleanup(); }
};

void ensure_socket_runtime() {
  static const WinsockRuntime runtime;
  (void)runtime;
}

[[nodiscard]] std::string last_socket_error() {
  return "Winsock error " + std::to_string(WSAGetLastError());
}

void close_socket(NativeSocket socket) {
  if (socket != kInvalidSocket) {
    closesocket(socket);
  }
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

void ensure_socket_runtime() {}

[[nodiscard]] std::string last_socket_error() { return std::strerror(errno); }

void close_socket(NativeSocket socket) {
  if (socket != kInvalidSocket) {
    close(socket);
  }
}
#endif

[[nodiscard]] std::string port_string(std::uint16_t port) { return std::to_string(port); }

class SocketHandle {
public:
  SocketHandle() = default;
  explicit SocketHandle(NativeSocket socket) : socket_(socket) {}
  ~SocketHandle() { reset(); }

  SocketHandle(const SocketHandle&) = delete;
  auto operator=(const SocketHandle&) -> SocketHandle& = delete;

  SocketHandle(SocketHandle&& other) noexcept
      : socket_(std::exchange(other.socket_, kInvalidSocket)) {}

  auto operator=(SocketHandle&& other) noexcept -> SocketHandle& {
    if (this != &other) {
      reset();
      socket_ = std::exchange(other.socket_, kInvalidSocket);
    }
    return *this;
  }

  [[nodiscard]] NativeSocket get() const { return socket_; }
  [[nodiscard]] bool valid() const { return socket_ != kInvalidSocket; }

  [[nodiscard]] NativeSocket release() { return std::exchange(socket_, kInvalidSocket); }

  void reset(NativeSocket socket = kInvalidSocket) {
    if (socket_ != kInvalidSocket) {
      close_socket(socket_);
    }
    socket_ = socket;
  }

private:
  NativeSocket socket_{kInvalidSocket};
};

[[nodiscard]] addrinfo hints_for(bool passive) {
  auto hints = addrinfo{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  if (passive) {
    hints.ai_flags = AI_PASSIVE;
  }
  return hints;
}

class ResolvedAddresses {
public:
  ResolvedAddresses(const TcpEndpoint& endpoint, bool passive) {
    ensure_socket_runtime();
    auto hints = hints_for(passive);
    const auto host = passive && endpoint.host.empty() ? nullptr : endpoint.host.c_str();
    const auto status = getaddrinfo(host, port_string(endpoint.port).c_str(), &hints, &result_);
    if (status != 0) {
#if defined(_WIN32)
      throw std::runtime_error("getaddrinfo failed: " + std::to_string(status));
#else
      throw std::runtime_error(std::string{"getaddrinfo failed: "} + gai_strerror(status));
#endif
    }
  }

  ~ResolvedAddresses() {
    if (result_ != nullptr) {
      freeaddrinfo(result_);
    }
  }

  ResolvedAddresses(const ResolvedAddresses&) = delete;
  auto operator=(const ResolvedAddresses&) -> ResolvedAddresses& = delete;

  [[nodiscard]] addrinfo* begin() const { return result_; }

private:
  addrinfo* result_{nullptr};
};

[[nodiscard]] ResolvedAddresses resolve_endpoint(const TcpEndpoint& endpoint, bool passive) {
  ensure_socket_runtime();
  return ResolvedAddresses{endpoint, passive};
}

void set_reuse_address(NativeSocket socket) {
  const auto enabled = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
             sizeof(enabled));
}

void set_no_delay(NativeSocket socket) {
  const auto enabled = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
             sizeof(enabled));
}

void set_socket_buffer(NativeSocket socket, int option, std::uint32_t bytes) {
  if (bytes == 0) {
    return;
  }

  const auto value = static_cast<int>(std::min<std::uint32_t>(bytes, 1024U * 1024U * 1024U));
  setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
}

void tune_socket(NativeSocket socket, const TcpEndpoint& endpoint) {
  set_no_delay(socket);
  set_socket_buffer(socket, SO_SNDBUF, endpoint.send_buffer_bytes);
  set_socket_buffer(socket, SO_RCVBUF, endpoint.receive_buffer_bytes);
}

[[nodiscard]] SocketHandle connect_socket(const TcpEndpoint& endpoint) {
  auto addresses = resolve_endpoint(endpoint, false);

  for (const auto* address = addresses.begin(); address != nullptr; address = address->ai_next) {
    auto socket =
        SocketHandle{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
    if (!socket.valid()) {
      continue;
    }

    if (::connect(socket.get(), address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
      tune_socket(socket.get(), endpoint);
      return socket;
    }
  }

  throw std::runtime_error("connect failed: " + last_socket_error());
}

[[nodiscard]] SocketHandle listen_socket(const TcpEndpoint& endpoint) {
  auto addresses = resolve_endpoint(endpoint, true);

  for (const auto* address = addresses.begin(); address != nullptr; address = address->ai_next) {
    auto socket =
        SocketHandle{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
    if (!socket.valid()) {
      continue;
    }

    set_reuse_address(socket.get());
    if (::bind(socket.get(), address->ai_addr, static_cast<int>(address->ai_addrlen)) != 0) {
      continue;
    }

    if (::listen(socket.get(), 1) != 0) {
      continue;
    }

    return socket;
  }

  throw std::runtime_error("listen failed: " + last_socket_error());
}

enum class TcpMode {
  Client,
  Server,
};

class TcpTransport final : public Transport {
public:
  TcpTransport(TcpMode mode, TcpEndpoint endpoint) : mode_(mode), endpoint_(std::move(endpoint)) {}

  [[nodiscard]] std::vector<PeerInfo> discover(const AutoConfigOptions& /*options*/) override {
    auto display = std::string{};
    if (mode_ == TcpMode::Client) {
      display = "TCP peer " + endpoint_.host + ":" + port_string(endpoint_.port);
    } else {
      display = "TCP listener " + endpoint_.host + ":" + port_string(endpoint_.port);
    }

    return {PeerInfo{
        .id = display,
        .display_name = display,
        .usb_speed = UsbSpeed::Unknown,
        .theoretical_bits_per_second = 0,
        .supports_high_speed = true,
        .supports_super_speed = true,
    }};
  }

  void connect(const PeerInfo& /*peer*/, const NegotiatedConfig& /*config*/) override {
    if (mode_ == TcpMode::Client) {
      data_socket_ = connect_socket(endpoint_);
      return;
    }

    listen_socket_ = listen_socket(endpoint_);
    auto accepted = SocketHandle{::accept(listen_socket_.get(), nullptr, nullptr)};
    if (!accepted.valid()) {
      throw std::runtime_error("accept failed: " + last_socket_error());
    }
    tune_socket(accepted.get(), endpoint_);
    data_socket_ = std::move(accepted);
  }

  [[nodiscard]] std::size_t send(std::span<const std::byte> payload) override {
    if (!data_socket_.valid()) {
      throw std::logic_error("TCP transport is not connected");
    }

    auto sent = std::size_t{0};
    while (sent < payload.size()) {
      const auto remaining = payload.size() - sent;
      const auto chunk = std::min<std::size_t>(remaining, 1024 * 1024);
      const auto* data = reinterpret_cast<const char*>(payload.data() + sent);
#if defined(_WIN32)
      const auto result = ::send(data_socket_.get(), data, static_cast<int>(chunk), 0);
#else
      const auto result = ::send(data_socket_.get(), data, chunk, 0);
#endif
      if (result <= 0) {
        throw std::runtime_error("send failed: " + last_socket_error());
      }
      sent += static_cast<std::size_t>(result);
    }

    return sent;
  }

  [[nodiscard]] std::vector<std::byte> receive(std::size_t max_bytes) override {
    auto buffer = std::vector<std::byte>(max_bytes);
    const auto received = receive_into(buffer);
    buffer.resize(received);
    return buffer;
  }

  [[nodiscard]] std::size_t receive_into(std::span<std::byte> buffer) override {
    if (!data_socket_.valid()) {
      throw std::logic_error("TCP transport is not connected");
    }

#if defined(_WIN32)
    const auto result = ::recv(data_socket_.get(), reinterpret_cast<char*>(buffer.data()),
                               static_cast<int>(buffer.size()), 0);
#else
    const auto result =
        ::recv(data_socket_.get(), reinterpret_cast<char*>(buffer.data()), buffer.size(), 0);
#endif
    if (result < 0) {
      throw std::runtime_error("receive failed: " + last_socket_error());
    }

    return static_cast<std::size_t>(result);
  }

  void close() noexcept override {
    data_socket_.reset();
    listen_socket_.reset();
  }

private:
  TcpMode mode_;
  TcpEndpoint endpoint_;
  SocketHandle data_socket_;
  SocketHandle listen_socket_;
};

} // namespace

std::unique_ptr<Transport> make_tcp_client_transport(TcpEndpoint endpoint) {
  return std::make_unique<TcpTransport>(TcpMode::Client, std::move(endpoint));
}

std::unique_ptr<Transport> make_tcp_server_transport(TcpEndpoint bind_endpoint) {
  return std::make_unique<TcpTransport>(TcpMode::Server, std::move(bind_endpoint));
}

} // namespace hyperlink
