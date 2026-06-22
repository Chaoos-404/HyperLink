#include "hyperlink/network_transport.hpp"
#include "hyperlink/session.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct BenchOptions {
  bool server{false};
  std::string host{"127.0.0.1"};
  std::uint16_t port{47777};
  std::uint32_t seconds{10};
  std::uint32_t parallel{1};
  std::uint32_t socket_buffer_bytes{0};
  std::size_t chunk_bytes{1024 * 1024};
};

void print_usage() {
  std::cout << "Usage:\n"
            << "  hyperlink_bench --server [--host 0.0.0.0] [--port 47777] [--parallel 1] "
               "[--socket-buffer-bytes 0]\n"
            << "  hyperlink_bench --client --host <peer-ip> [--port 47777] [--seconds 10] "
               "[--chunk-bytes 1048576] [--parallel 1] [--socket-buffer-bytes 0]\n";
}

std::uint16_t parse_port(const std::string& value) {
  const auto parsed = std::stoul(value);
  if (parsed == 0 || parsed > 65535) {
    throw std::invalid_argument("port must be between 1 and 65535");
  }
  return static_cast<std::uint16_t>(parsed);
}

BenchOptions parse_args(int argc, char** argv) {
  auto options = BenchOptions{};

  for (int index = 1; index < argc; ++index) {
    const auto arg = std::string_view{argv[index]};
    const auto require_value = [&](std::string_view name) -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(std::string{name} + " requires a value");
      }
      return argv[++index];
    };

    if (arg == "--server") {
      options.server = true;
      options.host = "0.0.0.0";
    } else if (arg == "--client") {
      options.server = false;
    } else if (arg == "--host") {
      options.host = require_value(arg);
    } else if (arg == "--port") {
      options.port = parse_port(require_value(arg));
    } else if (arg == "--seconds") {
      options.seconds = static_cast<std::uint32_t>(std::stoul(require_value(arg)));
      if (options.seconds == 0) {
        throw std::invalid_argument("seconds must be positive");
      }
    } else if (arg == "--parallel") {
      options.parallel = static_cast<std::uint32_t>(std::stoul(require_value(arg)));
      if (options.parallel == 0) {
        throw std::invalid_argument("parallel must be positive");
      }
    } else if (arg == "--chunk-bytes") {
      options.chunk_bytes = static_cast<std::size_t>(std::stoull(require_value(arg)));
      if (options.chunk_bytes == 0) {
        throw std::invalid_argument("chunk-bytes must be positive");
      }
    } else if (arg == "--socket-buffer-bytes") {
      options.socket_buffer_bytes = static_cast<std::uint32_t>(std::stoul(require_value(arg)));
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string{arg});
    }
  }

  if (static_cast<std::uint32_t>(options.port) + options.parallel - 1U > 65535U) {
    throw std::invalid_argument("port plus parallel stream count exceeds 65535");
  }

  return options;
}

double gib_per_second(std::uint64_t bytes, std::chrono::duration<double> elapsed) {
  if (elapsed.count() == 0.0) {
    return 0.0;
  }
  return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) / elapsed.count();
}

double gbits_per_second(std::uint64_t bytes, std::chrono::duration<double> elapsed) {
  if (elapsed.count() == 0.0) {
    return 0.0;
  }
  return static_cast<double>(bytes) * 8.0 / 1'000'000'000.0 / elapsed.count();
}

void print_result(std::uint64_t bytes, std::chrono::duration<double> elapsed) {
  std::cout << "Transferred " << bytes << " bytes in " << elapsed.count() << "s\n"
            << "Throughput: " << gbits_per_second(bytes, elapsed) << " Gbps"
            << " (" << gib_per_second(bytes, elapsed) << " GiB/s)\n";
}

hyperlink::Session connect_session(std::unique_ptr<hyperlink::Transport> transport) {
  auto session = hyperlink::Session{std::move(transport)};
  const auto peers = session.discover(hyperlink::AutoConfigOptions{});
  if (peers.empty()) {
    throw std::runtime_error("transport returned no peers");
  }
  session.connect(peers.front(), hyperlink::AutoConfigOptions{}, hyperlink::AutoConfigOptions{},
                  "hyperlink-bench");
  return session;
}

hyperlink::TcpEndpoint endpoint_for_stream(const BenchOptions& options, std::uint32_t stream) {
  return hyperlink::TcpEndpoint{
      .host = options.host,
      .port = static_cast<std::uint16_t>(options.port + stream),
      .send_buffer_bytes = options.socket_buffer_bytes,
      .receive_buffer_bytes = options.socket_buffer_bytes,
  };
}

struct StreamResult {
  std::uint64_t bytes{0};
  std::chrono::duration<double> elapsed{0};
  std::exception_ptr error;
};

void rethrow_first_error(const std::vector<StreamResult>& results) {
  for (const auto& result : results) {
    if (result.error) {
      std::rethrow_exception(result.error);
    }
  }
}

std::chrono::duration<double> max_elapsed(const std::vector<StreamResult>& results) {
  auto elapsed = std::chrono::duration<double>{0};
  for (const auto& result : results) {
    if (result.elapsed > elapsed) {
      elapsed = result.elapsed;
    }
  }
  return elapsed;
}

std::uint64_t total_bytes(const std::vector<StreamResult>& results) {
  auto bytes = std::uint64_t{0};
  for (const auto& result : results) {
    bytes += result.bytes;
  }
  return bytes;
}

int run_server(const BenchOptions& options) {
  std::cout << "Listening on " << options.host << ":" << options.port;
  if (options.parallel > 1) {
    std::cout << ".." << (options.port + options.parallel - 1);
  }
  std::cout << " with " << options.parallel << " stream(s)\n";

  auto results = std::vector<StreamResult>(options.parallel);
  auto threads = std::vector<std::thread>{};
  threads.reserve(options.parallel);

  for (std::uint32_t stream = 0; stream < options.parallel; ++stream) {
    threads.emplace_back([&, stream] {
      try {
        auto session = connect_session(
            hyperlink::make_tcp_server_transport(endpoint_for_stream(options, stream)));
        auto buffer = std::vector<std::byte>(options.chunk_bytes);

        const auto started = std::chrono::steady_clock::now();
        while (true) {
          const auto bytes = session.receive_into(buffer);
          if (bytes == 0) {
            break;
          }
          results[stream].bytes += bytes;
        }
        results[stream].elapsed = std::chrono::steady_clock::now() - started;
      } catch (...) {
        results[stream].error = std::current_exception();
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  rethrow_first_error(results);
  print_result(total_bytes(results), max_elapsed(results));
  return 0;
}

int run_client(const BenchOptions& options) {
  std::cout << "Connecting to " << options.host << ":" << options.port;
  if (options.parallel > 1) {
    std::cout << ".." << (options.port + options.parallel - 1);
  }
  std::cout << " with " << options.parallel << " stream(s)\n";

  auto results = std::vector<StreamResult>(options.parallel);
  auto threads = std::vector<std::thread>{};
  threads.reserve(options.parallel);

  for (std::uint32_t stream = 0; stream < options.parallel; ++stream) {
    threads.emplace_back([&, stream] {
      try {
        auto session = connect_session(
            hyperlink::make_tcp_client_transport(endpoint_for_stream(options, stream)));
        auto payload = std::vector<std::byte>(options.chunk_bytes, std::byte{0xA5});

        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::chrono::seconds{options.seconds};
        while (std::chrono::steady_clock::now() < deadline) {
          results[stream].bytes +=
              session.send(std::span<const std::byte>{payload.data(), payload.size()});
        }

        session.close();
        results[stream].elapsed = std::chrono::steady_clock::now() - started;
      } catch (...) {
        results[stream].error = std::current_exception();
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  rethrow_first_error(results);
  print_result(total_bytes(results), max_elapsed(results));
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_args(argc, argv);
    return options.server ? run_server(options) : run_client(options);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n\n";
    print_usage();
    return 1;
  }
}
