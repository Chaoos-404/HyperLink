#pragma once

#include "hyperlink/auto_config.hpp"
#include "hyperlink/export.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hyperlink {

enum class UsbSpeed {
  Unknown,
  LowSpeed,
  FullSpeed,
  HighSpeed,
  SuperSpeed,
  SuperSpeedPlus,
  Usb4_20Gbps,
  Usb4_40Gbps,
  Usb4_80Gbps,
};

struct HYPERLINK_API PeerInfo {
  std::string id;
  std::string display_name;
  std::uint16_t vendor_id{0};
  std::uint16_t product_id{0};
  UsbSpeed usb_speed{UsbSpeed::Unknown};
  std::uint64_t theoretical_bits_per_second{0};
  bool supports_high_speed{false};
  bool supports_super_speed{false};
};

class HYPERLINK_API Transport {
public:
  virtual ~Transport() = default;

  [[nodiscard]] virtual std::vector<PeerInfo> discover(const AutoConfigOptions& options) = 0;
  virtual void connect(const PeerInfo& peer, const NegotiatedConfig& config) = 0;
  [[nodiscard]] virtual std::size_t send(std::span<const std::byte> payload) = 0;
  [[nodiscard]] virtual std::vector<std::byte> receive(std::size_t max_bytes) = 0;
  [[nodiscard]] virtual std::size_t receive_into(std::span<std::byte> buffer) = 0;
  virtual void close() noexcept = 0;
};

class HYPERLINK_API Session {
public:
  explicit Session(std::unique_ptr<Transport> transport);
  ~Session();

  Session(const Session&) = delete;
  auto operator=(const Session&) -> Session& = delete;

  Session(Session&&) noexcept;
  auto operator=(Session&&) noexcept -> Session&;

  [[nodiscard]] std::vector<PeerInfo> discover(const AutoConfigOptions& options);
  void connect(const PeerInfo& peer, const AutoConfigOptions& local_options,
               const AutoConfigOptions& peer_options, std::string session_name);
  [[nodiscard]] std::size_t send(std::span<const std::byte> payload);
  [[nodiscard]] std::vector<std::byte> receive(std::size_t max_bytes);
  [[nodiscard]] std::size_t receive_into(std::span<std::byte> buffer);
  void close() noexcept;

private:
  std::unique_ptr<Transport> transport_;
  bool connected_{false};
};

[[nodiscard]] HYPERLINK_API std::unique_ptr<Transport> make_libusb_transport();
[[nodiscard]] HYPERLINK_API std::string to_string(UsbSpeed speed);

} // namespace hyperlink
