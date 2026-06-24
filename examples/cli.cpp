#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr auto kDefaultTransferPort = std::string_view{"47790"};
constexpr auto kDefaultDiscoveryPort = std::string_view{"47789"};
constexpr auto kDefaultParallel = std::string_view{"8"};

struct ReceiveOptions {
  std::string host{"0.0.0.0"};
  std::string port{kDefaultTransferPort};
  std::string output_dir;
  std::string parallel{kDefaultParallel};
  std::string discovery_port{kDefaultDiscoveryPort};
  bool advertise{true};
};

struct SendOptions {
  std::string file;
  std::string host;
  std::string port{kDefaultTransferPort};
  std::string parallel{kDefaultParallel};
  std::string discovery_port{kDefaultDiscoveryPort};
  std::string remote_name;
};

void print_usage() {
  std::cout
      << "Usage:\n"
      << "  hyperlink receive [--out <dir>] [--parallel 8]\n"
      << "  hyperlink send <file-or-folder> [--name <remote-name>]\n"
      << "  hyperlink bench [hyperlink_bench args...]\n"
      << "  hyperlink probe\n\n"
      << "Advanced:\n"
      << "  hyperlink receive [--host 0.0.0.0] [--port 47790] "
         "[--discovery-port 47789] [--no-advertise]\n"
      << "  hyperlink send <file-or-folder> [--host <peer-ip>] [--port 47790] "
         "[--parallel 8] [--discovery-port 47789]\n";
}

std::string default_download_dir() {
#if defined(_WIN32)
  if (const auto* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    return (std::filesystem::path{profile} / "Downloads").string();
  }
#else
  if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return (std::filesystem::path{home} / "Downloads").string();
  }
#endif
  return ".";
}

std::string require_value(int& index, int argc, char** argv, std::string_view option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string{option} + " requires a value");
  }
  return argv[++index];
}

ReceiveOptions parse_receive(int argc, char** argv, int start_index) {
  auto options = ReceiveOptions{};
  options.output_dir = default_download_dir();

  for (auto index = start_index; index < argc; ++index) {
    const auto arg = std::string_view{argv[index]};
    if (arg == "--out" || arg == "--output-dir") {
      options.output_dir = require_value(index, argc, argv, arg);
    } else if (arg == "--host") {
      options.host = require_value(index, argc, argv, arg);
    } else if (arg == "--port") {
      options.port = require_value(index, argc, argv, arg);
    } else if (arg == "--parallel") {
      options.parallel = require_value(index, argc, argv, arg);
    } else if (arg == "--discovery-port") {
      options.discovery_port = require_value(index, argc, argv, arg);
    } else if (arg == "--no-advertise") {
      options.advertise = false;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown receive option: " + std::string{arg});
    }
  }

  return options;
}

SendOptions parse_send(int argc, char** argv, int start_index) {
  auto options = SendOptions{};

  for (auto index = start_index; index < argc; ++index) {
    const auto arg = std::string_view{argv[index]};
    if (arg == "--name") {
      options.remote_name = require_value(index, argc, argv, arg);
    } else if (arg == "--host") {
      options.host = require_value(index, argc, argv, arg);
    } else if (arg == "--port") {
      options.port = require_value(index, argc, argv, arg);
    } else if (arg == "--parallel") {
      options.parallel = require_value(index, argc, argv, arg);
    } else if (arg == "--discovery-port") {
      options.discovery_port = require_value(index, argc, argv, arg);
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else if (!arg.empty() && arg.front() == '-') {
      throw std::invalid_argument("unknown send option: " + std::string{arg});
    } else if (options.file.empty()) {
      options.file = argv[index];
    } else {
      throw std::invalid_argument("send accepts exactly one file or folder path");
    }
  }

  if (options.file.empty()) {
    throw std::invalid_argument("send requires a file or folder path");
  }

  return options;
}

std::vector<std::string> passthrough_args(int argc, char** argv, int start_index) {
  auto args = std::vector<std::string>{};
  for (auto index = start_index; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  return args;
}

std::optional<std::filesystem::path> find_on_path(const std::string& executable) {
  const auto* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return std::nullopt;
  }

#if defined(_WIN32)
  constexpr auto separator = ';';
#else
  constexpr auto separator = ':';
#endif

  auto path = std::string_view{path_env};
  while (!path.empty()) {
    const auto next = path.find(separator);
    const auto entry = path.substr(0, next);
    if (!entry.empty()) {
      auto candidate = std::filesystem::path{std::string{entry}} / executable;
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
    }
    if (next == std::string_view::npos) {
      break;
    }
    path.remove_prefix(next + 1);
  }

  return std::nullopt;
}

std::filesystem::path sibling_tool(char** argv, const std::string& name) {
#if defined(_WIN32)
  const auto executable = name + ".exe";
#else
  const auto executable = name;
#endif

  auto self = std::filesystem::path{argv[0]};
  if (self.has_parent_path()) {
    auto candidate = std::filesystem::weakly_canonical(self).parent_path() / executable;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  auto local = std::filesystem::current_path() / executable;
  if (std::filesystem::exists(local)) {
    return local;
  }

  if (auto found = find_on_path(executable)) {
    return *found;
  }

  throw std::runtime_error("could not find helper executable: " + executable);
}

std::string shell_quote(const std::string& value) {
#if defined(_WIN32)
  auto quoted = std::string{"\""};
  for (const auto character : value) {
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
  for (const auto character : value) {
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

std::string shell_quote(const std::filesystem::path& path) { return shell_quote(path.string()); }

int run_command(const std::filesystem::path& executable, const std::vector<std::string>& args) {
  auto command = shell_quote(executable);
  for (const auto& arg : args) {
    command += " ";
    command += shell_quote(arg);
  }

  const auto result = std::system(command.c_str());
  return result == 0 ? 0 : 1;
}

int run_receive(char** argv, const ReceiveOptions& options) {
  auto args = std::vector<std::string>{
      "--receive", "--host", options.host, "--port", options.port, "--output-dir",
      options.output_dir, "--parallel", options.parallel, "--discovery-port",
      options.discovery_port,
  };
  if (!options.advertise) {
    args.emplace_back("--no-advertise");
  }
  return run_command(sibling_tool(argv, "hyperlink_file"), args);
}

int run_send(char** argv, const SendOptions& options) {
  auto args = std::vector<std::string>{"--send", "--file", options.file, "--discovery-port",
                                       options.discovery_port};
  if (options.host.empty()) {
    args.emplace_back("--auto");
  } else {
    args.emplace_back("--host");
    args.emplace_back(options.host);
    args.emplace_back("--port");
    args.emplace_back(options.port);
    args.emplace_back("--parallel");
    args.emplace_back(options.parallel);
  }

  if (!options.remote_name.empty()) {
    args.emplace_back("--name");
    args.emplace_back(options.remote_name);
  }

  return run_command(sibling_tool(argv, "hyperlink_file"), args);
}

int run_passthrough(char** argv, const std::string& executable,
                    const std::vector<std::string>& args) {
  return run_command(sibling_tool(argv, executable), args);
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      print_usage();
      return 1;
    }

    const auto command = std::string_view{argv[1]};
    if (command == "receive" || command == "recv") {
      return run_receive(argv, parse_receive(argc, argv, 2));
    }
    if (command == "send") {
      return run_send(argv, parse_send(argc, argv, 2));
    }
    if (command == "bench") {
      return run_passthrough(argv, "hyperlink_bench", passthrough_args(argc, argv, 2));
    }
    if (command == "probe") {
      return run_passthrough(argv, "hyperlink_probe", passthrough_args(argc, argv, 2));
    }
    if (command == "--help" || command == "-h" || command == "help") {
      print_usage();
      return 0;
    }

    throw std::invalid_argument("unknown command: " + std::string{command});
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n\n";
    print_usage();
    return 1;
  }
}
