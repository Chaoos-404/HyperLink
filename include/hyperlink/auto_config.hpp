#pragma once

#include "hyperlink/export.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace hyperlink {

enum class LinkMode {
  Host,
  Device,
  Auto,
};

enum class TransferProfile {
  LowLatency,
  Balanced,
  Throughput,
};

struct HYPERLINK_API AutoConfigOptions {
  LinkMode mode{LinkMode::Auto};
  TransferProfile profile{TransferProfile::Balanced};
  std::uint32_t preferred_packet_bytes{64 * 1024};
  std::chrono::milliseconds discovery_timeout{1500};
  bool require_encryption{true};
};

struct HYPERLINK_API NegotiatedConfig {
  LinkMode local_mode{LinkMode::Auto};
  TransferProfile profile{TransferProfile::Balanced};
  std::uint32_t packet_bytes{64 * 1024};
  bool encryption_enabled{true};
  std::string session_name;
};

[[nodiscard]] HYPERLINK_API std::optional<std::string> validate(const AutoConfigOptions& options);

[[nodiscard]] HYPERLINK_API NegotiatedConfig negotiate(const AutoConfigOptions& local,
                                                       const AutoConfigOptions& peer,
                                                       std::string session_name);

[[nodiscard]] HYPERLINK_API std::string to_string(LinkMode mode);
[[nodiscard]] HYPERLINK_API std::string to_string(TransferProfile profile);

} // namespace hyperlink
