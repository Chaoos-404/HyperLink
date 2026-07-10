#include "peer_discovery_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hyperlink {

namespace {

constexpr std::size_t kMaximumAdvertisementBytes = 512;
constexpr std::size_t kMaximumConcurrentProbeClients = 16;
constexpr std::string_view kAdvertisementPrefix = "HLINK_PEER_V2 ";
constexpr std::string_view kDiscoveryMessage = "HLINK_DISCOVER_V2";

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
using SocketLength = int;
using InetAddressLength = DWORD;

class WinsockRuntime {
public:
  WinsockRuntime() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw PeerDiscoveryError("WSAStartup failed");
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
using SocketLength = socklen_t;
using InetAddressLength = socklen_t;

void ensure_socket_runtime() {}

[[nodiscard]] std::string last_socket_error() { return std::strerror(errno); }

void close_socket(NativeSocket socket) {
  if (socket != kInvalidSocket) {
    close(socket);
  }
}
#endif

class SocketHandle {
public:
  explicit SocketHandle(NativeSocket socket = kInvalidSocket) : socket_(socket) {}
  ~SocketHandle() { close_socket(socket_); }
  SocketHandle(const SocketHandle&) = delete;
  auto operator=(const SocketHandle&) -> SocketHandle& = delete;
  SocketHandle(SocketHandle&& other) noexcept : socket_(std::exchange(other.socket_, kInvalidSocket)) {}
  auto operator=(SocketHandle&& other) noexcept -> SocketHandle& {
    if (this != &other) {
      close_socket(socket_);
      socket_ = std::exchange(other.socket_, kInvalidSocket);
    }
    return *this;
  }
  [[nodiscard]] NativeSocket get() const { return socket_; }
  [[nodiscard]] bool valid() const { return socket_ != kInvalidSocket; }
private:
  NativeSocket socket_;
};

struct DiscoveryTarget {
  std::string bind_address;
  std::string destination;
};

[[nodiscard]] std::string ipv4_string(const in_addr& address) {
  std::array<char, INET_ADDRSTRLEN> output{};
  return inet_ntop(AF_INET, &address, output.data(), static_cast<InetAddressLength>(output.size())) != nullptr
             ? std::string{output.data()}
             : std::string{};
}

void add_target(std::vector<DiscoveryTarget>& targets, std::string bind_address,
                std::string destination) {
  if (!bind_address.empty() && !destination.empty()) {
    targets.push_back({std::move(bind_address), std::move(destination)});
  }
}

[[nodiscard]] std::vector<DiscoveryTarget> enumerate_discovery_targets() {
  ensure_socket_runtime();
  auto targets = std::vector<DiscoveryTarget>{};
#if defined(_WIN32)
  ULONG size = 0;
  const auto first = GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &size);
  if (first == ERROR_BUFFER_OVERFLOW) {
    auto buffer = std::vector<std::byte>(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size) == NO_ERROR) {
      for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
          const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
          if (address == nullptr || address->sin_family != AF_INET) continue;
          const auto host = ipv4_string(address->sin_addr);
          const auto prefix = std::min<ULONG>(unicast->OnLinkPrefixLength, 32);
          const auto mask = prefix == 0 ? 0U : 0xFFFFFFFFU << (32 - prefix);
          const auto host_value = ntohl(address->sin_addr.s_addr);
          const auto broadcast = htonl((host_value & mask) | ~mask);
          add_target(targets, host, ipv4_string(in_addr{broadcast}));
          add_target(targets, host, "255.255.255.255");
        }
      }
    }
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) == 0) {
    for (auto* item = interfaces; item != nullptr; item = item->ifa_next) {
      if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
          (item->ifa_flags & IFF_UP) == 0) continue;
      const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
      const auto host = ipv4_string(address->sin_addr);
      in_addr broadcast{};
      if (item->ifa_broadaddr != nullptr) {
        broadcast = reinterpret_cast<const sockaddr_in*>(item->ifa_broadaddr)->sin_addr;
      } else if (item->ifa_netmask != nullptr) {
        const auto mask = reinterpret_cast<const sockaddr_in*>(item->ifa_netmask)->sin_addr.s_addr;
        broadcast.s_addr = address->sin_addr.s_addr | ~mask;
      }
      add_target(targets, host, ipv4_string(broadcast));
      add_target(targets, host, "255.255.255.255");
    }
    freeifaddrs(interfaces);
  }
#endif
  add_target(targets, "127.0.0.1", "127.0.0.1");
  std::ranges::sort(targets, {}, [](const DiscoveryTarget& target) {
    return std::tie(target.bind_address, target.destination);
  });
  targets.erase(std::unique(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
    return left.bind_address == right.bind_address && left.destination == right.destination;
  }), targets.end());
  return targets;
}

[[nodiscard]] sockaddr_in ipv4_endpoint(std::string_view host, std::uint16_t port) {
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  if (inet_pton(AF_INET, std::string{host}.c_str(), &endpoint.sin_addr) != 1) {
    throw PeerDiscoveryError("invalid IPv4 address: " + std::string{host});
  }
  return endpoint;
}

[[nodiscard]] bool discovery_datagram_within_limit(std::size_t received_bytes) {
  return received_bytes <= kMaximumAdvertisementBytes;
}

template <typename TargetProvider>
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
receive_discovery_replies(const PeerDiscoveryOptions& options, TargetProvider&& targets) {
  auto sockets = std::vector<SocketHandle>{};
  for (const auto& target : targets()) {
    auto socket = SocketHandle{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (!socket.valid()) continue;
    const auto enabled = 1;
    setsockopt(socket.get(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    const auto bind = ipv4_endpoint(target.bind_address, 0);
    const auto destination = ipv4_endpoint(target.destination, options.discovery_port);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&bind), sizeof(bind)) != 0 ||
        ::sendto(socket.get(), kDiscoveryMessage.data(), static_cast<int>(kDiscoveryMessage.size()), 0,
                 reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) < 0) continue;
    sockets.push_back(std::move(socket));
  }

  auto replies = std::vector<std::pair<std::string, std::string>>{};
  const auto deadline = std::chrono::steady_clock::now() + options.discovery_timeout;
  while (!sockets.empty() && std::chrono::steady_clock::now() < deadline) {
    fd_set readable;
    FD_ZERO(&readable);
    NativeSocket highest = 0;
    for (const auto& socket : sockets) { FD_SET(socket.get(), &readable); highest = std::max(highest, socket.get()); }
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(remaining.count() / 1000000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remaining.count() % 1000000);
    const auto ready = select(static_cast<int>(highest + 1), &readable, nullptr, nullptr, &timeout);
    if (ready <= 0) break;
    for (const auto& socket : sockets) {
      if (!FD_ISSET(socket.get(), &readable)) continue;
      std::array<char, kMaximumAdvertisementBytes + 1> buffer{};
      sockaddr_in source{};
      SocketLength source_length = sizeof(source);
      const auto received = recvfrom(socket.get(), buffer.data(), static_cast<int>(buffer.size()), 0,
                                     reinterpret_cast<sockaddr*>(&source), &source_length);
      if (received > 0 && discovery_datagram_within_limit(static_cast<std::size_t>(received))) {
        replies.emplace_back(std::string{buffer.data(), static_cast<std::size_t>(received)},
                             ipv4_string(source.sin_addr));
      }
    }
  }
  return replies;
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>>
receive_discovery_replies(const PeerDiscoveryOptions& options) {
  return receive_discovery_replies(options, enumerate_discovery_targets);
}

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
  if (left.endpoint.transfer_port != right.endpoint.transfer_port) {
    return left.endpoint.transfer_port < right.endpoint.transfer_port;
  }
  return left.endpoint.probe_port < right.endpoint.probe_port;
}

[[nodiscard]] std::vector<DiscoveredPeer> deduplicate_candidates(
    const std::vector<std::pair<std::string, std::string>>& replies) {
  auto candidates = std::vector<DiscoveredPeer>{};
  auto keys = std::set<std::tuple<std::string, std::uint16_t, std::uint16_t>>{};
  for (const auto& [message, source] : replies) {
    const auto peer = parse_peer_advertisement_for_testing(message, source);
    if (peer && keys.emplace(peer->endpoint.host, peer->endpoint.transfer_port,
                             peer->endpoint.probe_port).second) {
      candidates.push_back(*peer);
    }
  }
  return candidates;
}

void set_nonblocking(NativeSocket socket) {
#if defined(_WIN32)
  u_long enabled = 1;
  if (ioctlsocket(socket, FIONBIO, &enabled) != 0) {
    throw std::runtime_error("could not configure TCP probe: " + last_socket_error());
  }
#else
  const auto flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) {
    throw std::runtime_error("could not configure TCP probe: " + last_socket_error());
  }
#endif
}

void wait_for_socket(NativeSocket socket, bool write, std::chrono::steady_clock::time_point deadline) {
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(socket, &ready);
  const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::microseconds::zero()) throw std::runtime_error("probe timed out");
  timeval timeout{};
  timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(remaining.count() / 1000000);
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remaining.count() % 1000000);
  const auto result = select(static_cast<int>(socket + 1), write ? nullptr : &ready,
                             write ? &ready : nullptr, nullptr, &timeout);
  if (result == 0) throw std::runtime_error("probe timed out");
  if (result < 0) throw std::runtime_error("probe wait failed: " + last_socket_error());
}

[[nodiscard]] double probe_tcp_receive(const DiscoveredPeer& peer,
                                       const PeerDiscoveryOptions& options) {
  ensure_socket_runtime();
  auto socket = SocketHandle{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  if (!socket.valid()) throw std::runtime_error("probe socket failed: " + last_socket_error());
  set_nonblocking(socket.get());
  const auto endpoint = ipv4_endpoint(peer.endpoint.host, peer.endpoint.probe_port);
  const auto deadline = std::chrono::steady_clock::now() + options.probe_timeout;
  if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
#if defined(_WIN32)
    const auto pending = WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS;
#else
    const auto pending = errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
    if (!pending) throw std::runtime_error("probe connect failed: " + last_socket_error());
    wait_for_socket(socket.get(), true, deadline);
    int error = 0;
    SocketLength error_length = sizeof(error);
    if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_length) != 0 || error != 0) {
#if defined(_WIN32)
      throw std::runtime_error("probe connect failed: Winsock error " + std::to_string(error));
#else
      throw std::runtime_error("probe connect failed: " + std::string{std::strerror(error)});
#endif
    }
  }
  const auto started = std::chrono::steady_clock::now();
  auto received = std::size_t{0};
  auto buffer = std::array<std::byte, 64 * 1024>{};
  while (received < options.probe_payload_bytes) {
    wait_for_socket(socket.get(), false, deadline);
    const auto capacity = std::min(buffer.size(), static_cast<std::size_t>(options.probe_payload_bytes - received));
    const auto result = recv(socket.get(), reinterpret_cast<char*>(buffer.data()), static_cast<int>(capacity), 0);
    if (result < 0) {
#if defined(_WIN32)
      if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
      if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
#endif
      throw std::runtime_error("probe receive failed: " + last_socket_error());
    }
    if (result == 0) break;
    received += static_cast<std::size_t>(result);
  }
  if (received == 0) throw std::runtime_error("probe received no data");
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  return static_cast<double>(received) / std::max(elapsed, 1e-9);
}

template <typename Probe>
[[nodiscard]] DiscoveredPeer rank_candidates(
    const std::vector<std::pair<std::string, std::string>>& replies, Probe&& probe) {
  auto candidates = deduplicate_candidates(replies);
  if (candidates.empty()) throw PeerDiscoveryError("no Hyperlink peer discovered");
  auto failures = std::vector<std::string>{};
  auto successful = std::vector<DiscoveredPeer>{};
  for (auto& candidate : candidates) {
    try {
      candidate.measured_bytes_per_second = probe(candidate);
      successful.push_back(candidate);
    } catch (const std::exception& error) {
      failures.push_back(candidate.endpoint.host + ": " + error.what());
    }
  }
  if (successful.empty()) {
    auto message = std::string{"all Hyperlink peer probes failed"};
    for (const auto& failure : failures) message += "; " + failure;
    throw PeerDiscoveryError(message);
  }
  return select_fastest_peer_for_testing(std::move(successful));
}

template <typename ReplyProvider, typename Probe>
[[nodiscard]] DiscoveredPeer discover_and_rank(const PeerDiscoveryOptions& options,
                                               ReplyProvider&& replies, Probe&& probe) {
  return rank_candidates(replies(options), std::forward<Probe>(probe));
}

} // namespace

class PeerDiscoveryResponder::Impl {
public:
  explicit Impl(PeerDiscoveryResponderOptions options) : options_(std::move(options)) {}

  ~Impl() { stop(); }

  void start() {
    if (running_.exchange(true)) return;
    try {
      if (options_.transfer_port == 0 || options_.probe_port == 0 ||
          options_.discovery_port == 0 || options_.parallel_streams == 0 ||
          options_.display_name.empty()) {
        throw PeerDiscoveryError("invalid peer discovery responder options");
      }
      const auto last_transfer_port = static_cast<std::uint64_t>(options_.transfer_port) +
                                      static_cast<std::uint64_t>(options_.parallel_streams) - 1ULL;
      if (last_transfer_port > 65535U) {
        throw PeerDiscoveryError("transfer port plus parallel stream count exceeds 65535");
      }
      selected_probe_port_ = options_.probe_port;
      if (selected_probe_port_ >= options_.transfer_port &&
          selected_probe_port_ <= last_transfer_port) {
        if (last_transfer_port < 65535U) {
          selected_probe_port_ = static_cast<std::uint16_t>(last_transfer_port + 1U);
        } else if (options_.transfer_port > 1) {
          selected_probe_port_ = static_cast<std::uint16_t>(options_.transfer_port - 1U);
        } else {
          throw PeerDiscoveryError("no probe port is available outside the transfer stream range");
        }
      }
      advertisement_ = std::string{kAdvertisementPrefix} + std::to_string(options_.transfer_port) +
                       " " + std::to_string(selected_probe_port_) + " " +
                       std::to_string(options_.parallel_streams) + " " + options_.display_name;
      if (advertisement_.size() > kMaximumAdvertisementBytes) {
        throw PeerDiscoveryError("peer discovery advertisement exceeds 512 bytes");
      }
      ensure_socket_runtime();
      auto discovery_socket = make_udp_socket();
      auto probe_socket = make_probe_socket();
      discovery_thread_ = std::thread{[this, socket = std::move(discovery_socket)]() mutable {
        serve_discovery(std::move(socket));
      }};
      probe_thread_ = std::thread{[this, socket = std::move(probe_socket)]() mutable {
        serve_probes(std::move(socket));
      }};
    } catch (...) {
      stop();
      throw;
    }
  }

  void stop() {
    running_ = false;
    if (discovery_thread_.joinable()) discovery_thread_.join();
    if (probe_thread_.joinable()) probe_thread_.join();
  }

private:
  [[nodiscard]] SocketHandle make_udp_socket() const {
    auto socket = SocketHandle{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (!socket.valid()) throw PeerDiscoveryError("discovery responder socket failed: " + last_socket_error());
    const auto enabled = 1;
    setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    const auto endpoint = ipv4_endpoint(options_.bind_host, options_.discovery_port);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
      throw PeerDiscoveryError("discovery responder bind failed: " + last_socket_error());
    }
    return socket;
  }

  [[nodiscard]] SocketHandle make_probe_socket() const {
    auto socket = SocketHandle{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket.valid()) throw PeerDiscoveryError("probe responder socket failed: " + last_socket_error());
    const auto enabled = 1;
    setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    const auto endpoint = ipv4_endpoint(options_.bind_host, selected_probe_port_);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0 ||
        ::listen(socket.get(), SOMAXCONN) != 0) {
      throw PeerDiscoveryError("probe responder bind failed: " + last_socket_error());
    }
    set_nonblocking(socket.get());
    return socket;
  }

  void serve_discovery(SocketHandle socket) {
    while (running_) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(socket.get(), &readable);
      auto timeout = timeval{.tv_sec = 0, .tv_usec = 100000};
      if (select(static_cast<int>(socket.get() + 1), &readable, nullptr, nullptr, &timeout) <= 0 ||
          !FD_ISSET(socket.get(), &readable)) continue;
      std::array<char, kMaximumAdvertisementBytes> buffer{};
      sockaddr_in source{};
      SocketLength source_length = sizeof(source);
      const auto received = recvfrom(socket.get(), buffer.data(), static_cast<int>(buffer.size()), 0,
                                     reinterpret_cast<sockaddr*>(&source), &source_length);
      if (received <= 0 || std::string_view{buffer.data(), static_cast<std::size_t>(received)} != kDiscoveryMessage) continue;
      sendto(socket.get(), advertisement_.data(), static_cast<int>(advertisement_.size()), 0,
             reinterpret_cast<const sockaddr*>(&source), source_length);
    }
  }

  void serve_probes(SocketHandle socket) {
    struct ActiveProbe {
      SocketHandle socket;
      std::chrono::steady_clock::time_point deadline;
    };

    auto active_clients = std::vector<ActiveProbe>{};
    while (running_) {
      fd_set readable;
      fd_set writable;
      FD_ZERO(&readable);
      FD_ZERO(&writable);
      FD_SET(socket.get(), &readable);
      auto highest = socket.get();
      for (const auto& client : active_clients) {
        FD_SET(client.socket.get(), &writable);
        highest = std::max(highest, client.socket.get());
      }
      auto timeout = timeval{.tv_sec = 0, .tv_usec = 100000};
      const auto ready = select(static_cast<int>(highest + 1), &readable, &writable, nullptr,
                                &timeout);
      if (ready > 0 && FD_ISSET(socket.get(), &readable)) {
        while (running_) {
          sockaddr_in source{};
          SocketLength source_length = sizeof(source);
          auto client = SocketHandle{
              accept(socket.get(), reinterpret_cast<sockaddr*>(&source), &source_length)};
          if (!client.valid()) break;
          if (active_clients.size() >= kMaximumConcurrentProbeClients) continue;
          set_nonblocking(client.get());
          active_clients.push_back(
              {.socket = std::move(client),
               .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1}});
        }
      }
      const auto now = std::chrono::steady_clock::now();
      active_clients.erase(std::remove_if(active_clients.begin(), active_clients.end(),
                                          [&](const ActiveProbe& client) {
                                            return client.deadline <= now;
                                          }),
                           active_clients.end());
      if (ready > 0) {
        active_clients.erase(std::remove_if(active_clients.begin(), active_clients.end(),
                                            [&](const ActiveProbe& client) {
                                              return FD_ISSET(client.socket.get(), &writable) &&
                                                     !send_probe_bytes(client.socket.get());
                                            }),
                             active_clients.end());
      }
    }
  }

  [[nodiscard]] bool send_probe_bytes(NativeSocket client) const {
    static const auto buffer = std::array<std::byte, 64 * 1024>{};
    const auto sent = send(client, reinterpret_cast<const char*>(buffer.data()),
                           static_cast<int>(buffer.size()),
#if defined(_WIN32)
                           0
#else
                           MSG_NOSIGNAL
#endif
    );
    if (sent > 0) return true;
    if (sent == 0) return false;
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
  }

  PeerDiscoveryResponderOptions options_;
  std::uint16_t selected_probe_port_{0};
  std::string advertisement_;
  std::atomic_bool running_{false};
  std::thread discovery_thread_;
  std::thread probe_thread_;
};

PeerDiscoveryResponder::PeerDiscoveryResponder(PeerDiscoveryResponderOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
PeerDiscoveryResponder::~PeerDiscoveryResponder() = default;
PeerDiscoveryResponder::PeerDiscoveryResponder(PeerDiscoveryResponder&&) noexcept = default;
auto PeerDiscoveryResponder::operator=(PeerDiscoveryResponder&&) noexcept -> PeerDiscoveryResponder& = default;
void PeerDiscoveryResponder::start() { impl_->start(); }
void PeerDiscoveryResponder::stop() { impl_->stop(); }

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

std::vector<DiscoveredPeer> PeerDiscovery::discover(const PeerDiscoveryOptions& options) {
  const auto candidates = deduplicate_candidates(receive_discovery_replies(options));
  if (candidates.empty()) throw PeerDiscoveryError("no Hyperlink peer discovered");
  return candidates;
}

DiscoveredPeer PeerDiscovery::select_fastest(const PeerDiscoveryOptions& options) {
  return discover_and_rank(options, [](const PeerDiscoveryOptions& discovery_options) {
                             return receive_discovery_replies(discovery_options);
                           },
                           [&options](const DiscoveredPeer& peer) {
                             return probe_tcp_receive(peer, options);
                           });
}

std::unique_ptr<Transport> PeerDiscovery::connect_fastest(const PeerDiscoveryOptions& options,
                                                           TcpEndpoint endpoint) {
  return make_tcp_client_transport(detail::selected_tcp_endpoint_for_testing(endpoint,
                                                                              select_fastest(options)));
}

TcpEndpoint detail::selected_tcp_endpoint_for_testing(TcpEndpoint endpoint,
                                                       const DiscoveredPeer& peer) {
  endpoint.host = peer.endpoint.host;
  endpoint.port = peer.endpoint.transfer_port;
  return endpoint;
}

bool detail::discovery_datagram_within_limit_for_testing(std::size_t received_bytes) {
  return discovery_datagram_within_limit(received_bytes);
}

DiscoveredPeer detail::PeerDiscoveryTestHarness::select_fastest() {
  return discover_and_rank(PeerDiscoveryOptions{}, [this](const PeerDiscoveryOptions&) -> const auto& {
    return replies;
  }, [this](const DiscoveredPeer& peer) {
    const auto key = detail::ProbeEndpointKey{peer.endpoint.host, peer.endpoint.probe_port};
    probed_endpoints.push_back(key);
    if (const auto failure = probe_failures.find(key); failure != probe_failures.end()) {
      throw std::runtime_error(failure->second);
    }
    if (const auto rate = probe_rates.find(key); rate != probe_rates.end()) return rate->second;
    throw std::runtime_error("no probe rate configured");
  });
}

} // namespace hyperlink
