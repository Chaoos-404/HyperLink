#include "hyperlink/network_transport.hpp"
#include "hyperlink/session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr auto kDefaultPort = std::uint16_t{47790};
constexpr auto kDefaultDiscoveryPort = std::uint16_t{47789};
constexpr auto kChunkBytes = std::size_t{4 * 1024 * 1024};
constexpr auto kMagic = std::array<char, 8>{'H', 'L', 'X', 'F', 'E', 'R', '3', '\0'};
constexpr auto kHeaderBytes = std::size_t{48};
constexpr auto kMaxNameBytes = std::uint64_t{4096};
constexpr auto kStreamingPayloadSize = std::numeric_limits<std::uint64_t>::max();

enum class TransferKind : std::uint64_t {
  File = 1,
  DirectoryTar = 2,
  FilePart = 3,
  DirectoryEntry = 4,
  DirectoryFile = 5,
  DirectorySymlink = 6,
};

struct Options {
  bool send{false};
  bool receive{false};
  bool auto_discover{false};
  bool allow_lan{false};
  bool advertise{true};
  std::string host{"0.0.0.0"};
  std::uint16_t port{kDefaultPort};
  std::uint16_t discovery_port{kDefaultDiscoveryPort};
  std::uint32_t parallel{1};
  std::filesystem::path file;
  std::filesystem::path output_dir{"."};
  std::string remote_name;
};

void print_usage() {
  std::cout << "Usage:\n"
            << "  hyperlink_file --receive [--host 0.0.0.0] [--port 47790] "
               "[--output-dir .] [--parallel 1] [--no-advertise]\n"
            << "  hyperlink_file --send --host <peer-ip> --file <path> [--port 47790] "
               "[--name <remote-name>] [--parallel 1]\n"
            << "  hyperlink_file --send --auto --file <path> [--name <remote-name>] "
               "[--allow-lan]\n";
}

std::uint16_t parse_port(const std::string& value) {
  const auto parsed = std::stoul(value);
  if (parsed == 0 || parsed > 65535) {
    throw std::invalid_argument("port must be between 1 and 65535");
  }
  return static_cast<std::uint16_t>(parsed);
}

Options parse_args(int argc, char** argv) {
  auto options = Options{};

  for (int index = 1; index < argc; ++index) {
    const auto arg = std::string_view{argv[index]};
    const auto require_value = [&](std::string_view name) -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(std::string{name} + " requires a value");
      }
      return argv[++index];
    };

    if (arg == "--send") {
      options.send = true;
      options.host = "127.0.0.1";
    } else if (arg == "--receive") {
      options.receive = true;
      options.host = "0.0.0.0";
    } else if (arg == "--auto") {
      options.auto_discover = true;
      options.host.clear();
    } else if (arg == "--allow-lan") {
      options.allow_lan = true;
    } else if (arg == "--no-advertise") {
      options.advertise = false;
    } else if (arg == "--host") {
      options.host = require_value(arg);
    } else if (arg == "--port") {
      options.port = parse_port(require_value(arg));
    } else if (arg == "--discovery-port") {
      options.discovery_port = parse_port(require_value(arg));
    } else if (arg == "--parallel") {
      options.parallel = static_cast<std::uint32_t>(std::stoul(require_value(arg)));
      if (options.parallel == 0) {
        throw std::invalid_argument("parallel must be positive");
      }
    } else if (arg == "--file") {
      options.file = require_value(arg);
    } else if (arg == "--output-dir") {
      options.output_dir = require_value(arg);
    } else if (arg == "--name") {
      options.remote_name = require_value(arg);
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string{arg});
    }
  }

  if (options.send == options.receive) {
    throw std::invalid_argument("choose exactly one of --send or --receive");
  }

  if (options.send && options.file.empty()) {
    throw std::invalid_argument("--send requires --file");
  }
  if (options.receive && options.auto_discover) {
    throw std::invalid_argument("--auto is only valid with --send");
  }

  if (static_cast<std::uint32_t>(options.port) + options.parallel - 1U > 65535U) {
    throw std::invalid_argument("port plus parallel stream count exceeds 65535");
  }

  return options;
}

#if defined(_WIN32)
using UdpSocket = SOCKET;
constexpr UdpSocket kInvalidUdpSocket = INVALID_SOCKET;

class SocketRuntime {
public:
  SocketRuntime() {
    auto data = WSADATA{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }

  ~SocketRuntime() { WSACleanup(); }
};

void ensure_socket_runtime() {
  static const SocketRuntime runtime;
  (void)runtime;
}

void close_udp_socket(UdpSocket socket) {
  if (socket != kInvalidUdpSocket) {
    closesocket(socket);
  }
}

std::string socket_error() { return "Winsock error " + std::to_string(WSAGetLastError()); }
#else
using UdpSocket = int;
constexpr UdpSocket kInvalidUdpSocket = -1;

void ensure_socket_runtime() {}

void close_udp_socket(UdpSocket socket) {
  if (socket != kInvalidUdpSocket) {
    close(socket);
  }
}

std::string socket_error() { return std::strerror(errno); }
#endif

class UdpSocketHandle {
public:
  UdpSocketHandle() = default;
  explicit UdpSocketHandle(UdpSocket socket) : socket_(socket) {}
  ~UdpSocketHandle() { reset(); }

  UdpSocketHandle(const UdpSocketHandle&) = delete;
  auto operator=(const UdpSocketHandle&) -> UdpSocketHandle& = delete;

  UdpSocketHandle(UdpSocketHandle&& other) noexcept
      : socket_(std::exchange(other.socket_, kInvalidUdpSocket)) {}

  auto operator=(UdpSocketHandle&& other) noexcept -> UdpSocketHandle& {
    if (this != &other) {
      reset();
      socket_ = std::exchange(other.socket_, kInvalidUdpSocket);
    }
    return *this;
  }

  [[nodiscard]] UdpSocket get() const { return socket_; }
  [[nodiscard]] bool valid() const { return socket_ != kInvalidUdpSocket; }

  void reset(UdpSocket socket = kInvalidUdpSocket) {
    if (socket_ != kInvalidUdpSocket) {
      close_udp_socket(socket_);
    }
    socket_ = socket;
  }

private:
  UdpSocket socket_{kInvalidUdpSocket};
};

void set_socket_timeout(UdpSocket socket, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
  const auto value = static_cast<DWORD>(timeout.count());
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
  auto value = timeval{
      .tv_sec = static_cast<time_t>(timeout.count() / 1000),
      .tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000),
  };
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
}

void set_reuse_address(UdpSocket socket) {
  const auto enabled = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
             sizeof(enabled));
}

void set_broadcast(UdpSocket socket) {
  const auto enabled = 1;
  setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled),
             sizeof(enabled));
}

sockaddr_in make_ipv4_address(const std::string& host, std::uint16_t port) {
  auto address = sockaddr_in{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    throw std::runtime_error("invalid IPv4 address: " + host);
  }
  return address;
}

std::string ipv4_to_string(const sockaddr_in& address) {
  auto buffer = std::array<char, INET_ADDRSTRLEN>{};
  const auto* result = inet_ntop(AF_INET, &address.sin_addr, buffer.data(), buffer.size());
  if (result == nullptr) {
    return {};
  }
  return buffer.data();
}

struct DiscoveredReceiver {
  std::string host;
  std::uint16_t port{kDefaultPort};
  std::uint32_t parallel{1};
  std::string name;
};

struct DiscoveryTarget {
  std::string bind_host;
  std::string destination_host;
};

struct ActiveDiscoverySocket {
  UdpSocketHandle socket;
  std::string bind_host;
};

int discovery_preference(const std::string& host) {
  auto octets = std::array<int, 4>{};
  if (std::sscanf(host.c_str(), "%d.%d.%d.%d", &octets[0], &octets[1], &octets[2],
                  &octets[3]) != 4) {
    return 1000;
  }

  if (octets[0] == 169 && octets[1] == 254) {
    return 0;
  }
  if (octets[0] == 10) {
    return 100;
  }
  if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) {
    return 110;
  }
  if (octets[0] == 192 && octets[1] == 168) {
    return 120;
  }
  if (octets[0] == 127) {
    return 900;
  }
  return 200;
}

std::string local_receiver_name() {
  auto buffer = std::array<char, 256>{};
  if (gethostname(buffer.data(), static_cast<int>(buffer.size() - 1)) == 0) {
    return buffer.data();
  }
  return "hyperlink-receiver";
}

std::optional<DiscoveredReceiver> parse_discovery_response(std::string_view message,
                                                           const std::string& source_host) {
  auto stream = std::istringstream{std::string{message}};
  auto magic = std::string{};
  auto port = std::uint32_t{0};
  auto parallel = std::uint32_t{0};
  auto name = std::string{};
  stream >> magic >> port >> parallel >> name;
  if (magic != "HLINK_FILE_V1" || port == 0 || port > 65535 || parallel == 0) {
    return std::nullopt;
  }
  return DiscoveredReceiver{
      .host = source_host,
      .port = static_cast<std::uint16_t>(port),
      .parallel = parallel,
      .name = name.empty() ? source_host : name,
  };
}

bool is_link_local_ipv4(const std::string& host) { return discovery_preference(host) == 0; }

std::string candidate_hosts(const std::vector<DiscoveredReceiver>& candidates) {
  auto hosts = std::string{};
  for (const auto& candidate : candidates) {
    if (!hosts.empty()) {
      hosts += ", ";
    }
    hosts += candidate.host;
  }
  return hosts.empty() ? "none" : hosts;
}

std::string ipv4_from_host_order(std::uint32_t address) {
  auto socket_address = sockaddr_in{};
  socket_address.sin_family = AF_INET;
  socket_address.sin_addr.s_addr = htonl(address);
  return ipv4_to_string(socket_address);
}

std::uint32_t ipv4_to_host_order(const sockaddr_in& address) {
  return ntohl(address.sin_addr.s_addr);
}

std::uint32_t prefix_mask(std::uint32_t prefix_length) {
  if (prefix_length == 0) {
    return 0;
  }
  if (prefix_length >= 32) {
    return 0xFFFFFFFFU;
  }
  return 0xFFFFFFFFU << (32U - prefix_length);
}

void add_discovery_target(std::vector<DiscoveryTarget>& targets, std::string bind_host,
                          std::string destination_host) {
  if (bind_host.empty() || destination_host.empty()) {
    return;
  }
  const auto duplicate = std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
    return target.bind_host == bind_host && target.destination_host == destination_host;
  });
  if (!duplicate) {
    targets.push_back(DiscoveryTarget{
        .bind_host = std::move(bind_host),
        .destination_host = std::move(destination_host),
    });
  }
}

#if defined(_WIN32)
std::vector<DiscoveryTarget> interface_discovery_targets() {
  auto targets = std::vector<DiscoveryTarget>{};
  auto buffer_size = ULONG{15 * 1024};
  auto buffer = std::vector<unsigned char>(buffer_size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  auto flags = ULONG{GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER};
  auto result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
  if (result == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(buffer_size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
  }
  if (result != NO_ERROR) {
    return targets;
  }

  for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) {
      continue;
    }
    for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
         unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      const auto* address =
          reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
      const auto host = ipv4_to_string(*address);
      const auto local = ipv4_to_host_order(*address);
      const auto mask = prefix_mask(unicast->OnLinkPrefixLength);
      const auto broadcast = (local & mask) | (~mask);
      add_discovery_target(targets, host, ipv4_from_host_order(broadcast));
      add_discovery_target(targets, host, "255.255.255.255");
    }
  }
  return targets;
}
#else
std::vector<DiscoveryTarget> interface_discovery_targets() {
  auto targets = std::vector<DiscoveryTarget>{};
  auto* interfaces = static_cast<ifaddrs*>(nullptr);
  if (getifaddrs(&interfaces) != 0) {
    return targets;
  }

  for (auto* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
    if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((interface->ifa_flags & IFF_UP) == 0) {
      continue;
    }

    const auto* address = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr);
    const auto bind_host = ipv4_to_string(*address);
    if (interface->ifa_broadaddr != nullptr && (interface->ifa_flags & IFF_BROADCAST) != 0) {
      const auto* broadcast = reinterpret_cast<const sockaddr_in*>(interface->ifa_broadaddr);
      add_discovery_target(targets, bind_host, ipv4_to_string(*broadcast));
    } else if (interface->ifa_netmask != nullptr) {
      const auto* netmask = reinterpret_cast<const sockaddr_in*>(interface->ifa_netmask);
      const auto broadcast =
          (ipv4_to_host_order(*address) & ipv4_to_host_order(*netmask)) |
          (~ipv4_to_host_order(*netmask));
      add_discovery_target(targets, bind_host, ipv4_from_host_order(broadcast));
    }
    add_discovery_target(targets, bind_host, "255.255.255.255");
  }

  freeifaddrs(interfaces);
  return targets;
}
#endif

std::vector<ActiveDiscoverySocket> open_discovery_sockets(
    const std::vector<DiscoveryTarget>& targets, std::uint16_t discovery_port,
    std::string_view request) {
  auto sockets = std::vector<ActiveDiscoverySocket>{};
  for (const auto& target : targets) {
    auto socket = UdpSocketHandle{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (!socket.valid()) {
      continue;
    }
    set_broadcast(socket.get());
    set_socket_timeout(socket.get(), std::chrono::milliseconds{100});

    auto bind_address = make_ipv4_address(target.bind_host, 0);
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&bind_address),
             sizeof(bind_address)) != 0) {
      continue;
    }

    const auto destination = make_ipv4_address(target.destination_host, discovery_port);
    static_cast<void>(sendto(socket.get(), request.data(), static_cast<int>(request.size()), 0,
                             reinterpret_cast<const sockaddr*>(&destination),
                             sizeof(destination)));
    sockets.push_back(ActiveDiscoverySocket{
        .socket = std::move(socket),
        .bind_host = target.bind_host,
    });
  }
  return sockets;
}

void collect_discovery_responses(UdpSocket socket, std::vector<DiscoveredReceiver>& candidates) {
  auto buffer = std::array<char, 512>{};
  auto source = sockaddr_in{};
#if defined(_WIN32)
  auto source_size = static_cast<int>(sizeof(source));
  const auto received = recvfrom(socket, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                                 reinterpret_cast<sockaddr*>(&source), &source_size);
#else
  auto source_size = socklen_t{sizeof(source)};
  const auto received = recvfrom(socket, buffer.data(), buffer.size() - 1, 0,
                                 reinterpret_cast<sockaddr*>(&source), &source_size);
#endif
  if (received <= 0) {
    return;
  }
  const auto source_host = ipv4_to_string(source);
  if (auto parsed = parse_discovery_response(
          std::string_view{buffer.data(), static_cast<std::size_t>(received)}, source_host)) {
    if (std::none_of(candidates.begin(), candidates.end(),
                     [&](const auto& candidate) { return candidate.host == parsed->host; })) {
      candidates.push_back(*parsed);
    }
  }
}

DiscoveredReceiver discover_receiver(std::uint16_t discovery_port,
                                     std::chrono::milliseconds timeout, bool allow_lan) {
  ensure_socket_runtime();

  const auto request = std::string{"HLINK_DISCOVER_V1 hyperlink-file"};
  auto targets = interface_discovery_targets();
  add_discovery_target(targets, "127.0.0.1", "127.0.0.1");

  auto active_sockets = open_discovery_sockets(targets, discovery_port, request);

  auto fallback_socket = UdpSocketHandle{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
  if (fallback_socket.valid()) {
    set_broadcast(fallback_socket.get());
    set_socket_timeout(fallback_socket.get(), std::chrono::milliseconds{100});
  }
  for (const auto& destination : {"255.255.255.255", "169.254.255.255", "127.0.0.1"}) {
    if (!fallback_socket.valid()) {
      continue;
    }
    const auto address = make_ipv4_address(destination, discovery_port);
    static_cast<void>(sendto(fallback_socket.get(), request.data(),
                             static_cast<int>(request.size()), 0,
                             reinterpret_cast<const sockaddr*>(&address), sizeof(address)));
  }

  if (active_sockets.empty() && !fallback_socket.valid()) {
    throw std::runtime_error("failed to create discovery socket: " + socket_error());
  }

  auto candidates = std::vector<DiscoveredReceiver>{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto first_response_deadline = std::optional<std::chrono::steady_clock::time_point>{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (fallback_socket.valid()) {
      collect_discovery_responses(fallback_socket.get(), candidates);
    }
    for (const auto& active_socket : active_sockets) {
      collect_discovery_responses(active_socket.socket.get(), candidates);
    }

    if (std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
          return discovery_preference(candidate.host) == 0;
        })) {
      break;
    }

    if (!candidates.empty()) {
      if (!first_response_deadline) {
        first_response_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{350};
      }
    }

    if (first_response_deadline && std::chrono::steady_clock::now() >= *first_response_deadline) {
      break;
    }
  }

  if (!candidates.empty()) {
    const auto best = *std::min_element(candidates.begin(), candidates.end(), [](const auto& left,
                                                                                 const auto& right) {
      return discovery_preference(left.host) < discovery_preference(right.host);
    });
    if (!allow_lan && !is_link_local_ipv4(best.host) && best.host.rfind("127.", 0) != 0) {
      throw std::runtime_error(
          "auto-discovery only found LAN/Wi-Fi receiver address(es): " +
          candidate_hosts(candidates) +
          ". Connect over USB4/Thunderbolt, then rerun with --host <receiver-169.254.x.x>. "
          "Use --allow-lan only if you intentionally want Wi-Fi/LAN.");
    }
    return best;
  }

  throw std::runtime_error("no Hyperlink receiver discovered");
}

class DiscoveryResponder {
public:
  DiscoveryResponder(const Options& options)
      : discovery_port_(options.discovery_port), transfer_port_(options.port),
        parallel_(options.parallel), name_(local_receiver_name()) {}

  ~DiscoveryResponder() { stop(); }

  void start() {
    running_ = true;
    thread_ = std::thread{[this] { run(); }};
  }

  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  void run() {
    try {
      ensure_socket_runtime();
      auto socket = UdpSocketHandle{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
      if (!socket.valid()) {
        return;
      }
      set_reuse_address(socket.get());
      set_socket_timeout(socket.get(), std::chrono::milliseconds{500});

      auto bind_address = sockaddr_in{};
      bind_address.sin_family = AF_INET;
      bind_address.sin_port = htons(discovery_port_);
      bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&bind_address),
               sizeof(bind_address)) != 0) {
        return;
      }

      const auto response = "HLINK_FILE_V1 " + std::to_string(transfer_port_) + " " +
                            std::to_string(parallel_) + " " + name_;
      while (running_) {
        auto buffer = std::array<char, 512>{};
        auto source = sockaddr_in{};
#if defined(_WIN32)
        auto source_size = static_cast<int>(sizeof(source));
        const auto received =
            recvfrom(socket.get(), buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                     reinterpret_cast<sockaddr*>(&source), &source_size);
#else
        auto source_size = socklen_t{sizeof(source)};
        const auto received = recvfrom(socket.get(), buffer.data(), buffer.size() - 1, 0,
                                       reinterpret_cast<sockaddr*>(&source), &source_size);
#endif
        if (received <= 0) {
          continue;
        }
        const auto request = std::string_view{buffer.data(), static_cast<std::size_t>(received)};
        if (request.find("HLINK_DISCOVER_V1") != 0) {
          continue;
        }
        sendto(socket.get(), response.data(), static_cast<int>(response.size()), 0,
               reinterpret_cast<const sockaddr*>(&source), source_size);
      }
    } catch (...) {
    }
  }

  std::uint16_t discovery_port_{kDefaultDiscoveryPort};
  std::uint16_t transfer_port_{kDefaultPort};
  std::uint32_t parallel_{1};
  std::string name_;
  std::atomic_bool running_{false};
  std::thread thread_;
};

void write_u64_be(std::span<std::byte> buffer, std::uint64_t value) {
  for (auto index = std::size_t{0}; index < 8; ++index) {
    buffer[index] = static_cast<std::byte>((value >> ((7 - index) * 8)) & 0xFF);
  }
}

std::uint64_t read_u64_be(std::span<const std::byte> buffer) {
  auto value = std::uint64_t{0};
  for (auto byte : buffer.subspan(0, 8)) {
    value = (value << 8) | static_cast<std::uint64_t>(byte);
  }
  return value;
}

std::vector<std::byte> make_header(TransferKind kind, std::uint64_t payload_size,
                                   std::uint64_t name_size, std::uint64_t offset,
                                   std::uint64_t total_size) {
  auto header = std::vector<std::byte>(kHeaderBytes);
  for (auto index = std::size_t{0}; index < kMagic.size(); ++index) {
    header[index] = static_cast<std::byte>(kMagic[index]);
  }
  write_u64_be(std::span<std::byte>{header}.subspan(8, 8), static_cast<std::uint64_t>(kind));
  write_u64_be(std::span<std::byte>{header}.subspan(16, 8), payload_size);
  write_u64_be(std::span<std::byte>{header}.subspan(24, 8), name_size);
  write_u64_be(std::span<std::byte>{header}.subspan(32, 8), offset);
  write_u64_be(std::span<std::byte>{header}.subspan(40, 8), total_size);
  return header;
}

void receive_exact(hyperlink::Session& session, std::span<std::byte> buffer) {
  auto offset = std::size_t{0};
  while (offset < buffer.size()) {
    const auto received = session.receive_into(buffer.subspan(offset));
    if (received == 0) {
      throw std::runtime_error("connection closed before expected bytes arrived");
    }
    offset += received;
  }
}

hyperlink::Session connect_session(std::unique_ptr<hyperlink::Transport> transport) {
  auto session = hyperlink::Session{std::move(transport)};
  const auto peers = session.discover(hyperlink::AutoConfigOptions{});
  if (peers.empty()) {
    throw std::runtime_error("transport returned no peers");
  }
  session.connect(peers.front(), hyperlink::AutoConfigOptions{}, hyperlink::AutoConfigOptions{},
                  "hyperlink-file");
  return session;
}

std::string safe_file_name(std::string name) {
  auto path = std::filesystem::path{std::move(name)}.filename().string();
  if (path.empty() || path == "." || path == "..") {
    throw std::runtime_error("invalid incoming file name");
  }
  return path;
}

std::filesystem::path safe_relative_path(const std::string& name) {
  auto path = std::filesystem::path{name};
  if (path.empty() || path.is_absolute()) {
    throw std::runtime_error("invalid incoming relative path");
  }

  for (const auto& part : path) {
    if (part == "." || part == ".." || part.empty()) {
      throw std::runtime_error("invalid incoming relative path");
    }
  }

  return path;
}

std::string wire_path(const std::filesystem::path& path) { return path.generic_string(); }

std::uint64_t permissions_value(std::filesystem::perms permissions) {
  return static_cast<std::uint64_t>(permissions) & 07777U;
}

void apply_permissions(const std::filesystem::path& path, std::uint64_t mode) {
#if defined(_WIN32)
  (void)path;
  (void)mode;
#else
  std::error_code error;
  std::filesystem::permissions(path, static_cast<std::filesystem::perms>(mode),
                               std::filesystem::perm_options::replace, error);
#endif
}

void create_symlink_or_fallback(const std::string& target,
                                const std::filesystem::path& output_path) {
  std::error_code error;
  std::filesystem::remove(output_path, error);
  error.clear();
  std::filesystem::create_symlink(target, output_path, error);
  if (!error) {
    return;
  }

  auto fallback = std::ofstream{output_path.string() + ".hyperlink-symlink.txt",
                                std::ios::binary | std::ios::trunc};
  if (!fallback) {
    throw std::runtime_error("failed to create symlink fallback file for: " + output_path.string());
  }
  fallback << target;
}

std::string shell_quote(const std::filesystem::path& path) {
  const auto text = path.string();
#if defined(_WIN32)
  auto quoted = std::string{"\""};
  for (const auto character : text) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
  auto quoted = std::string{"'"};
  for (const auto character : text) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

std::string tar_create_command(const std::filesystem::path& directory) {
  const auto parent =
      directory.parent_path().empty() ? std::filesystem::path{"."} : directory.parent_path();
  return "tar -cf - -C " + shell_quote(parent) + " " + shell_quote(directory.filename());
}

std::string tar_extract_command(const std::filesystem::path& output_dir) {
  return "tar -xf - -C " + shell_quote(output_dir);
}

std::FILE* open_pipe(const std::string& command, const char* mode) {
#if defined(_WIN32)
  return _popen(command.c_str(), mode);
#else
  return popen(command.c_str(), mode);
#endif
}

int close_pipe(std::FILE* pipe) {
#if defined(_WIN32)
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

void close_pipe_checked(std::FILE* pipe, const std::string& command) {
  const auto status = close_pipe(pipe);
  if (status != 0) {
    throw std::runtime_error("command failed: " + command);
  }
}

double gib_per_second(std::uint64_t bytes, std::chrono::duration<double> elapsed) {
  if (elapsed.count() == 0.0) {
    return 0.0;
  }
  return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) / elapsed.count();
}

void print_result(std::string_view verb, std::uint64_t bytes,
                  std::chrono::duration<double> elapsed) {
  std::cout << verb << " " << bytes << " bytes in " << elapsed.count() << "s"
            << " (" << gib_per_second(bytes, elapsed) << " GiB/s)\n";
}

hyperlink::TcpEndpoint endpoint_for_stream(const Options& options, std::uint32_t stream) {
  return hyperlink::TcpEndpoint{
      .host = options.host,
      .port = static_cast<std::uint16_t>(options.port + stream),
      .send_buffer_bytes = 8 * 1024 * 1024,
      .receive_buffer_bytes = 8 * 1024 * 1024,
  };
}

struct StreamResult {
  std::uint64_t bytes{0};
  std::exception_ptr error;
};

void rethrow_first_error(const std::vector<StreamResult>& results) {
  for (const auto& result : results) {
    if (result.error) {
      std::rethrow_exception(result.error);
    }
  }
}

std::uint64_t total_bytes(const std::vector<StreamResult>& results) {
  auto bytes = std::uint64_t{0};
  for (const auto& result : results) {
    bytes += result.bytes;
  }
  return bytes;
}

std::pair<std::uint64_t, std::uint64_t>
byte_range_for_stream(std::uint64_t total_size, std::uint32_t stream, std::uint32_t parallel) {
  const auto base = total_size / parallel;
  const auto extra = total_size % parallel;
  const auto offset = base * stream + std::min<std::uint64_t>(stream, extra);
  const auto size = base + (stream < extra ? 1 : 0);
  return {offset, size};
}

struct DirectoryItem {
  TransferKind kind{TransferKind::DirectoryEntry};
  std::filesystem::path source;
  std::string relative_path;
  std::uint64_t payload_size{0};
  std::uint64_t mode{0};
};

struct DirectoryPartition {
  std::vector<DirectoryItem> items;
  std::uint64_t bytes{0};
};

std::vector<DirectoryItem> collect_directory_items(const std::filesystem::path& directory) {
  auto items = std::vector<DirectoryItem>{};
  const auto root_parent =
      directory.parent_path().empty() ? std::filesystem::path{"."} : directory.parent_path();

  const auto add_item = [&](const std::filesystem::path& path) {
    const auto status = std::filesystem::symlink_status(path);
    const auto relative = wire_path(path.lexically_relative(root_parent));

    if (std::filesystem::is_symlink(status)) {
      const auto target = std::filesystem::read_symlink(path).string();
      items.push_back(DirectoryItem{
          .kind = TransferKind::DirectorySymlink,
          .source = path,
          .relative_path = relative,
          .payload_size = target.size(),
          .mode = 0,
      });
      return;
    }

    if (std::filesystem::is_directory(status)) {
      items.push_back(DirectoryItem{
          .kind = TransferKind::DirectoryEntry,
          .source = path,
          .relative_path = relative,
          .payload_size = 0,
          .mode = permissions_value(status.permissions()),
      });
      return;
    }

    if (std::filesystem::is_regular_file(status)) {
      items.push_back(DirectoryItem{
          .kind = TransferKind::DirectoryFile,
          .source = path,
          .relative_path = relative,
          .payload_size = std::filesystem::file_size(path),
          .mode = permissions_value(status.permissions()),
      });
    }
  };

  add_item(directory);
  for (const auto& entry : std::filesystem::recursive_directory_iterator{
           directory, std::filesystem::directory_options::skip_permission_denied}) {
    add_item(entry.path());
  }

  return items;
}

std::vector<DirectoryPartition> partition_directory_items(std::vector<DirectoryItem> items,
                                                          std::uint32_t parallel) {
  auto partitions = std::vector<DirectoryPartition>(parallel);

  auto metadata = std::vector<DirectoryItem>{};
  auto payload = std::vector<DirectoryItem>{};
  for (auto& item : items) {
    if (item.kind == TransferKind::DirectoryFile || item.kind == TransferKind::DirectorySymlink) {
      payload.push_back(std::move(item));
    } else {
      metadata.push_back(std::move(item));
    }
  }

  for (auto& item : metadata) {
    partitions.front().items.push_back(std::move(item));
  }

  std::sort(payload.begin(), payload.end(), [](const auto& left, const auto& right) {
    return left.payload_size > right.payload_size;
  });

  for (auto& item : payload) {
    auto target = std::min_element(
        partitions.begin(), partitions.end(),
        [](const auto& left, const auto& right) { return left.bytes < right.bytes; });
    target->bytes += item.payload_size;
    target->items.push_back(std::move(item));
  }

  return partitions;
}

void send_file_range(const Options& options, const std::filesystem::path& file_path,
                     const std::string& remote_name, std::uint64_t total_size, std::uint64_t offset,
                     std::uint64_t payload_size, std::uint32_t stream, StreamResult& result) {
  try {
    auto input = std::ifstream{file_path, std::ios::binary};
    if (!input) {
      throw std::runtime_error("failed to open input payload: " + file_path.string());
    }
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) {
      throw std::runtime_error("failed to seek input file");
    }

    std::cout << "Connecting stream " << stream << " to " << options.host << ":"
              << (options.port + stream) << '\n';
    auto session =
        connect_session(hyperlink::make_tcp_client_transport(endpoint_for_stream(options, stream)));

    const auto kind = options.parallel > 1 ? TransferKind::FilePart : TransferKind::File;
    const auto header = make_header(kind, payload_size, remote_name.size(), offset, total_size);
    static_cast<void>(session.send(header));
    static_cast<void>(
        session.send(std::as_bytes(std::span<const char>{remote_name.data(), remote_name.size()})));

    auto buffer = std::vector<std::byte>(kChunkBytes);
    while (result.bytes < payload_size) {
      const auto remaining = payload_size - result.bytes;
      const auto chunk = static_cast<std::streamsize>(
          std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
      input.read(reinterpret_cast<char*>(buffer.data()), chunk);
      const auto read = input.gcount();
      if (read <= 0) {
        throw std::runtime_error("input file ended unexpectedly");
      }

      static_cast<void>(
          session.send(std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(read)}));
      result.bytes += static_cast<std::uint64_t>(read);
    }

    session.close();
  } catch (...) {
    result.error = std::current_exception();
  }
}

void send_directory_items(const Options& options, const std::vector<DirectoryItem>& items,
                          std::uint32_t stream, StreamResult& result) {
  try {
    std::cout << "Connecting stream " << stream << " to " << options.host << ":"
              << (options.port + stream) << '\n';
    auto session =
        connect_session(hyperlink::make_tcp_client_transport(endpoint_for_stream(options, stream)));
    auto buffer = std::vector<std::byte>(kChunkBytes);

    for (const auto& item : items) {
      const auto header =
          make_header(item.kind, item.payload_size, item.relative_path.size(), item.mode, 0);
      static_cast<void>(session.send(header));
      static_cast<void>(session.send(std::as_bytes(
          std::span<const char>{item.relative_path.data(), item.relative_path.size()})));

      if (item.kind == TransferKind::DirectoryFile) {
        auto input = std::ifstream{item.source, std::ios::binary};
        if (!input) {
          throw std::runtime_error("failed to open input file: " + item.source.string());
        }

        auto sent_file = std::uint64_t{0};
        while (sent_file < item.payload_size) {
          const auto remaining = item.payload_size - sent_file;
          const auto chunk = static_cast<std::streamsize>(
              std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
          input.read(reinterpret_cast<char*>(buffer.data()), chunk);
          const auto read = input.gcount();
          if (read <= 0) {
            throw std::runtime_error("input file ended unexpectedly");
          }
          static_cast<void>(session.send(
              std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(read)}));
          sent_file += static_cast<std::uint64_t>(read);
          result.bytes += static_cast<std::uint64_t>(read);
        }
      } else if (item.kind == TransferKind::DirectorySymlink) {
        const auto target = std::filesystem::read_symlink(item.source).string();
        static_cast<void>(
            session.send(std::as_bytes(std::span<const char>{target.data(), target.size()})));
        result.bytes += target.size();
      }
    }

    session.close();
  } catch (...) {
    result.error = std::current_exception();
  }
}

int run_send(const Options& options) {
  auto effective_options = options;
  if (effective_options.auto_discover) {
    std::cout << "Discovering Hyperlink receiver on UDP port " << effective_options.discovery_port
              << "...\n";
    const auto receiver =
        discover_receiver(effective_options.discovery_port, std::chrono::milliseconds{2500},
                          effective_options.allow_lan);
    effective_options.host = receiver.host;
    effective_options.port = receiver.port;
    effective_options.parallel = receiver.parallel;
    std::cout << "Discovered " << receiver.name << " at " << receiver.host << ":" << receiver.port
              << " with " << receiver.parallel << " stream(s)\n";
    if (discovery_preference(receiver.host) > 0 && receiver.host.rfind("127.", 0) != 0) {
      std::cerr << "warning: discovered a non-link-local address. If this is using Wi-Fi/LAN, "
                   "rerun with --host <169.254.x.x> for the USB4/Thunderbolt link.\n";
    }
  }

  if (!std::filesystem::exists(effective_options.file)) {
    throw std::runtime_error("path does not exist: " + effective_options.file.string());
  }

  auto transfer_path = effective_options.file;
  auto kind = TransferKind::File;
  auto payload_size = std::uint64_t{0};

  if (std::filesystem::is_directory(effective_options.file)) {
    if (!effective_options.remote_name.empty()) {
      throw std::runtime_error("--name is not supported for directory transfers yet");
    }
    kind = TransferKind::DirectoryTar;
    payload_size = kStreamingPayloadSize;
  } else if (!std::filesystem::is_regular_file(effective_options.file)) {
    throw std::runtime_error("path is not a regular file or directory: " +
                             effective_options.file.string());
  } else {
    payload_size = std::filesystem::file_size(transfer_path);
  }

  auto remote_name = effective_options.remote_name.empty()
                         ? effective_options.file.filename().string()
                         : effective_options.remote_name;
  remote_name = safe_file_name(std::move(remote_name));

  auto sent = std::uint64_t{0};
  const auto started = std::chrono::steady_clock::now();

  if (std::filesystem::is_directory(effective_options.file) && effective_options.parallel > 1) {
    const auto items = collect_directory_items(effective_options.file);
    const auto partitions = partition_directory_items(items, effective_options.parallel);
    auto results = std::vector<StreamResult>(effective_options.parallel);
    auto threads = std::vector<std::thread>{};
    threads.reserve(effective_options.parallel);

    for (auto stream = std::uint32_t{0}; stream < effective_options.parallel; ++stream) {
      threads.emplace_back([&, stream] {
        send_directory_items(effective_options, partitions[stream].items, stream, results[stream]);
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    rethrow_first_error(results);
    sent = total_bytes(results);
  } else if (kind == TransferKind::DirectoryTar) {
    std::cout << "Connecting to " << effective_options.host << ":" << effective_options.port
              << '\n';
    auto session = connect_session(
        hyperlink::make_tcp_client_transport(endpoint_for_stream(effective_options, 0)));
    const auto header =
        make_header(kind, payload_size, remote_name.size(), 0, kStreamingPayloadSize);
    static_cast<void>(session.send(header));
    static_cast<void>(
        session.send(std::as_bytes(std::span<const char>{remote_name.data(), remote_name.size()})));

    auto buffer = std::vector<std::byte>(kChunkBytes);
    const auto command = tar_create_command(effective_options.file);
    auto* pipe = open_pipe(command, "r");
    if (pipe == nullptr) {
      throw std::runtime_error("failed to start command: " + command);
    }

    while (true) {
      const auto read = std::fread(buffer.data(), 1, buffer.size(), pipe);
      if (read > 0) {
        static_cast<void>(session.send(std::span<const std::byte>{buffer.data(), read}));
        sent += read;
      }

      if (read < buffer.size()) {
        if (std::ferror(pipe) != 0) {
          close_pipe(pipe);
          throw std::runtime_error("failed while reading directory archive stream");
        }
        break;
      }
    }
    close_pipe_checked(pipe, command);
    session.close();
  } else {
    auto results = std::vector<StreamResult>(effective_options.parallel);
    auto threads = std::vector<std::thread>{};
    threads.reserve(effective_options.parallel);

    for (auto stream = std::uint32_t{0}; stream < effective_options.parallel; ++stream) {
      const auto [offset, size] =
          byte_range_for_stream(payload_size, stream, effective_options.parallel);
      threads.emplace_back([&, stream, offset, size] {
        send_file_range(effective_options, transfer_path, remote_name, payload_size, offset, size,
                        stream, results[stream]);
      });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    rethrow_first_error(results);
    sent = total_bytes(results);
  }

  print_result("Sent", sent, std::chrono::steady_clock::now() - started);
  return 0;
}

void receive_payload_to_file(hyperlink::Session& session, const std::filesystem::path& output_path,
                             std::uint64_t payload_size) {
  auto output = std::ofstream{output_path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw std::runtime_error("failed to open output file: " + output_path.string());
  }

  auto buffer = std::vector<std::byte>(kChunkBytes);
  auto received_total = std::uint64_t{0};
  while (received_total < payload_size) {
    const auto remaining = payload_size - received_total;
    const auto want = std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size()));
    const auto received =
        session.receive_into(std::span<std::byte>{buffer.data(), static_cast<std::size_t>(want)});
    if (received == 0) {
      throw std::runtime_error("connection closed before transfer completed");
    }

    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(received));
    if (!output) {
      throw std::runtime_error("failed while writing output file");
    }
    received_total += received;
  }
}

std::uint64_t receive_stream_to_pipe(hyperlink::Session& session, const std::string& command) {
  auto* pipe = open_pipe(command, "w");
  if (pipe == nullptr) {
    throw std::runtime_error("failed to start command: " + command);
  }

  auto buffer = std::vector<std::byte>(kChunkBytes);
  auto received_total = std::uint64_t{0};
  while (true) {
    const auto received = session.receive_into(buffer);
    if (received == 0) {
      break;
    }

    const auto written = std::fwrite(buffer.data(), 1, received, pipe);
    if (written != received) {
      close_pipe(pipe);
      throw std::runtime_error("failed while writing archive stream to extractor");
    }
    received_total += received;
  }

  close_pipe_checked(pipe, command);
  return received_total;
}

struct IncomingHeader {
  TransferKind kind{TransferKind::File};
  std::uint64_t payload_size{0};
  std::uint64_t name_size{0};
  std::uint64_t offset{0};
  std::uint64_t total_size{0};
  std::string file_name;
};

IncomingHeader receive_header(hyperlink::Session& session) {
  auto header = std::vector<std::byte>(kHeaderBytes);
  receive_exact(session, header);
  for (auto index = std::size_t{0}; index < kMagic.size(); ++index) {
    if (header[index] != static_cast<std::byte>(kMagic[index])) {
      throw std::runtime_error("invalid file-transfer header");
    }
  }

  auto incoming = IncomingHeader{
      .kind =
          static_cast<TransferKind>(read_u64_be(std::span<const std::byte>{header}.subspan(8, 8))),
      .payload_size = read_u64_be(std::span<const std::byte>{header}.subspan(16, 8)),
      .name_size = read_u64_be(std::span<const std::byte>{header}.subspan(24, 8)),
      .offset = read_u64_be(std::span<const std::byte>{header}.subspan(32, 8)),
      .total_size = read_u64_be(std::span<const std::byte>{header}.subspan(40, 8)),
  };

  if (incoming.name_size == 0 || incoming.name_size > kMaxNameBytes) {
    throw std::runtime_error("invalid incoming file name length");
  }
  if (incoming.kind != TransferKind::File && incoming.kind != TransferKind::DirectoryTar &&
      incoming.kind != TransferKind::FilePart && incoming.kind != TransferKind::DirectoryEntry &&
      incoming.kind != TransferKind::DirectoryFile &&
      incoming.kind != TransferKind::DirectorySymlink) {
    throw std::runtime_error("unsupported incoming transfer kind");
  }

  auto name_bytes = std::vector<std::byte>(static_cast<std::size_t>(incoming.name_size));
  receive_exact(session, name_bytes);
  incoming.file_name = safe_file_name(
      std::string{reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size()});
  return incoming;
}

std::optional<IncomingHeader> receive_header_optional(hyperlink::Session& session) {
  auto header = std::vector<std::byte>(kHeaderBytes);
  auto offset = std::size_t{0};
  while (offset < header.size()) {
    const auto received = session.receive_into(std::span<std::byte>{header}.subspan(offset));
    if (received == 0) {
      if (offset == 0) {
        return std::nullopt;
      }
      throw std::runtime_error("connection closed in the middle of a header");
    }
    offset += received;
  }

  for (auto index = std::size_t{0}; index < kMagic.size(); ++index) {
    if (header[index] != static_cast<std::byte>(kMagic[index])) {
      throw std::runtime_error("invalid file-transfer header");
    }
  }

  auto incoming = IncomingHeader{
      .kind =
          static_cast<TransferKind>(read_u64_be(std::span<const std::byte>{header}.subspan(8, 8))),
      .payload_size = read_u64_be(std::span<const std::byte>{header}.subspan(16, 8)),
      .name_size = read_u64_be(std::span<const std::byte>{header}.subspan(24, 8)),
      .offset = read_u64_be(std::span<const std::byte>{header}.subspan(32, 8)),
      .total_size = read_u64_be(std::span<const std::byte>{header}.subspan(40, 8)),
  };

  if (incoming.name_size == 0 || incoming.name_size > kMaxNameBytes) {
    throw std::runtime_error("invalid incoming file name length");
  }
  if (incoming.kind != TransferKind::File && incoming.kind != TransferKind::DirectoryTar &&
      incoming.kind != TransferKind::FilePart && incoming.kind != TransferKind::DirectoryEntry &&
      incoming.kind != TransferKind::DirectoryFile &&
      incoming.kind != TransferKind::DirectorySymlink) {
    throw std::runtime_error("unsupported incoming transfer kind");
  }

  auto name_bytes = std::vector<std::byte>(static_cast<std::size_t>(incoming.name_size));
  receive_exact(session, name_bytes);
  incoming.file_name =
      std::string{reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size()};
  return incoming;
}

void receive_directory_file(hyperlink::Session& session, const std::filesystem::path& output_path,
                            const IncomingHeader& incoming, StreamResult& result) {
  std::error_code create_error;
  std::filesystem::create_directories(output_path.parent_path(), create_error);
  if (create_error) {
    throw std::runtime_error("failed to create parent directory: " +
                             output_path.parent_path().string() + ": " + create_error.message());
  }
  auto output = std::ofstream{output_path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw std::runtime_error("failed to open output file: " + output_path.string());
  }

  auto buffer = std::vector<std::byte>(kChunkBytes);
  auto received_total = std::uint64_t{0};
  while (received_total < incoming.payload_size) {
    const auto remaining = incoming.payload_size - received_total;
    const auto want = std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size()));
    const auto received =
        session.receive_into(std::span<std::byte>{buffer.data(), static_cast<std::size_t>(want)});
    if (received == 0) {
      throw std::runtime_error("connection closed before directory file completed");
    }
    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(received));
    if (!output) {
      throw std::runtime_error("failed while writing output file");
    }
    received_total += received;
    result.bytes += received;
  }

  apply_permissions(output_path, incoming.offset);
}

std::string receive_text_payload(hyperlink::Session& session, std::uint64_t payload_size) {
  auto bytes = std::vector<std::byte>(static_cast<std::size_t>(payload_size));
  receive_exact(session, bytes);
  return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct ReceiveSharedState {
  std::mutex mutex;
  bool initialized{false};
  std::filesystem::path output_path;
  std::string file_name;
  std::uint64_t total_size{0};
};

void receive_file_part(const Options& options, std::uint32_t stream, ReceiveSharedState& shared,
                       StreamResult& result) {
  try {
    auto session =
        connect_session(hyperlink::make_tcp_server_transport(endpoint_for_stream(options, stream)));

    while (true) {
      auto incoming = receive_header_optional(session);
      if (!incoming) {
        break;
      }

      if (incoming->kind == TransferKind::DirectoryEntry) {
        const auto output_path = options.output_dir / safe_relative_path(incoming->file_name);
        std::filesystem::create_directories(output_path);
        continue;
      }

      if (incoming->kind == TransferKind::DirectorySymlink) {
        const auto output_path = options.output_dir / safe_relative_path(incoming->file_name);
        std::filesystem::create_directories(output_path.parent_path());
        const auto target = receive_text_payload(session, incoming->payload_size);
        create_symlink_or_fallback(target, output_path);
        result.bytes += incoming->payload_size;
        continue;
      }

      if (incoming->kind == TransferKind::DirectoryFile) {
        const auto output_path = options.output_dir / safe_relative_path(incoming->file_name);
        receive_directory_file(session, output_path, *incoming, result);
        continue;
      }

      if (incoming->kind != TransferKind::FilePart && incoming->kind != TransferKind::File) {
        throw std::runtime_error("parallel receive got unsupported transfer kind");
      }
      incoming->file_name = safe_file_name(std::move(incoming->file_name));
      if (incoming->offset + incoming->payload_size > incoming->total_size) {
        throw std::runtime_error("incoming file range exceeds total file size");
      }

      std::filesystem::path output_path;
      {
        auto lock = std::lock_guard<std::mutex>{shared.mutex};
        if (!shared.initialized) {
          shared.initialized = true;
          shared.file_name = incoming->file_name;
          shared.total_size = incoming->total_size;
          shared.output_path = options.output_dir / incoming->file_name;
          {
            auto output = std::ofstream{shared.output_path, std::ios::binary | std::ios::trunc};
            if (!output) {
              throw std::runtime_error("failed to create output file: " +
                                       shared.output_path.string());
            }
          }
          std::filesystem::resize_file(shared.output_path, shared.total_size);
        } else if (shared.file_name != incoming->file_name ||
                   shared.total_size != incoming->total_size) {
          throw std::runtime_error("incoming file parts do not describe the same file");
        }
        output_path = shared.output_path;
      }

      auto output = std::fstream{output_path, std::ios::binary | std::ios::in | std::ios::out};
      if (!output) {
        throw std::runtime_error("failed to open output file: " + output_path.string());
      }
      output.seekp(static_cast<std::streamoff>(incoming->offset));
      if (!output) {
        throw std::runtime_error("failed to seek output file");
      }

      auto buffer = std::vector<std::byte>(kChunkBytes);
      auto received_part = std::uint64_t{0};
      while (received_part < incoming->payload_size) {
        const auto remaining = incoming->payload_size - received_part;
        const auto want =
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size()));
        const auto received = session.receive_into(
            std::span<std::byte>{buffer.data(), static_cast<std::size_t>(want)});
        if (received == 0) {
          throw std::runtime_error("connection closed before file part completed");
        }

        output.write(reinterpret_cast<const char*>(buffer.data()),
                     static_cast<std::streamsize>(received));
        if (!output) {
          throw std::runtime_error("failed while writing output file");
        }
        received_part += received;
        result.bytes += received;
      }
    }
  } catch (...) {
    result.error = std::current_exception();
  }
}

int run_receive(const Options& options) {
  std::filesystem::create_directories(options.output_dir);
  auto responder = std::optional<DiscoveryResponder>{};
  if (options.advertise) {
    responder.emplace(options);
    responder->start();
    std::cout << "Advertising receiver on UDP port " << options.discovery_port << '\n';
  }

  if (options.parallel > 1) {
    std::cout << "Listening on " << options.host << ":" << options.port << ".."
              << (options.port + options.parallel - 1) << " with " << options.parallel
              << " stream(s)\n";

    auto shared = ReceiveSharedState{};
    auto results = std::vector<StreamResult>(options.parallel);
    auto threads = std::vector<std::thread>{};
    threads.reserve(options.parallel);
    const auto started = std::chrono::steady_clock::now();

    for (auto stream = std::uint32_t{0}; stream < options.parallel; ++stream) {
      threads.emplace_back(
          [&, stream] { receive_file_part(options, stream, shared, results[stream]); });
    }

    for (auto& thread : threads) {
      thread.join();
    }

    rethrow_first_error(results);
    const auto received = total_bytes(results);
    print_result("Received", received, std::chrono::steady_clock::now() - started);
    if (shared.initialized) {
      std::cout << "Saved to " << shared.output_path << '\n';
    }
    return 0;
  }

  std::cout << "Listening on " << options.host << ":" << options.port << '\n';
  auto session =
      connect_session(hyperlink::make_tcp_server_transport(endpoint_for_stream(options, 0)));
  const auto incoming = receive_header(session);
  if (incoming.kind == TransferKind::FilePart) {
    throw std::runtime_error("incoming file uses parallel parts; restart receiver with --parallel");
  }

  std::cout << "Receiving " << incoming.file_name;
  if (incoming.payload_size == kStreamingPayloadSize) {
    std::cout << " (stream)\n";
  } else {
    std::cout << " (" << incoming.payload_size << " bytes)\n";
  }
  const auto started = std::chrono::steady_clock::now();

  if (incoming.kind == TransferKind::File) {
    const auto output_path = options.output_dir / incoming.file_name;
    receive_payload_to_file(session, output_path, incoming.payload_size);
    print_result("Received", incoming.payload_size, std::chrono::steady_clock::now() - started);
    std::cout << "Saved to " << output_path << '\n';
    return 0;
  }

  const auto received = receive_stream_to_pipe(session, tar_extract_command(options.output_dir));
  print_result("Received", received, std::chrono::steady_clock::now() - started);
  std::cout << "Extracted directory archive into " << options.output_dir << '\n';
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_args(argc, argv);
    return options.send ? run_send(options) : run_receive(options);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n\n";
    print_usage();
    return 1;
  }
}
