#include "hyperlink/session.hpp"

#include <libusb.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace hyperlink {

namespace {

[[nodiscard]] UsbSpeed usb_speed_from_libusb(int speed) {
  switch (speed) {
  case LIBUSB_SPEED_LOW:
    return UsbSpeed::LowSpeed;
  case LIBUSB_SPEED_FULL:
    return UsbSpeed::FullSpeed;
  case LIBUSB_SPEED_HIGH:
    return UsbSpeed::HighSpeed;
  case LIBUSB_SPEED_SUPER:
    return UsbSpeed::SuperSpeed;
  case LIBUSB_SPEED_SUPER_PLUS:
    return UsbSpeed::SuperSpeedPlus;
  case LIBUSB_SPEED_UNKNOWN:
  default:
    return UsbSpeed::Unknown;
  }
}

[[nodiscard]] std::uint64_t theoretical_bits_per_second(UsbSpeed speed) {
  switch (speed) {
  case UsbSpeed::LowSpeed:
    return 1'500'000;
  case UsbSpeed::FullSpeed:
    return 12'000'000;
  case UsbSpeed::HighSpeed:
    return 480'000'000;
  case UsbSpeed::SuperSpeed:
    return 5'000'000'000;
  case UsbSpeed::SuperSpeedPlus:
    return 10'000'000'000;
  case UsbSpeed::Usb4_20Gbps:
    return 20'000'000'000;
  case UsbSpeed::Usb4_40Gbps:
    return 40'000'000'000;
  case UsbSpeed::Usb4_80Gbps:
    return 80'000'000'000;
  case UsbSpeed::Unknown:
  default:
    return 0;
  }
}

class LibusbTransport final : public Transport {
public:
  LibusbTransport() {
    const auto result = libusb_init(&context_);
    if (result != LIBUSB_SUCCESS) {
      throw std::runtime_error("libusb_init failed");
    }
  }

  ~LibusbTransport() override {
    if (context_ != nullptr) {
      libusb_exit(context_);
    }
  }

  [[nodiscard]] std::vector<PeerInfo> discover(const AutoConfigOptions& /*options*/) override {
    libusb_device** devices = nullptr;
    const auto count = libusb_get_device_list(context_, &devices);
    if (count < 0) {
      throw std::runtime_error("libusb_get_device_list failed");
    }

    auto peers = std::vector<PeerInfo>{};
    peers.reserve(static_cast<std::size_t>(count));

    for (std::ptrdiff_t index = 0; index < count; ++index) {
      auto descriptor = libusb_device_descriptor{};
      if (libusb_get_device_descriptor(devices[index], &descriptor) != LIBUSB_SUCCESS) {
        continue;
      }

      const auto speed = usb_speed_from_libusb(libusb_get_device_speed(devices[index]));
      auto id = std::ostringstream{};
      id << "usb-" << static_cast<int>(libusb_get_bus_number(devices[index])) << "-"
         << static_cast<int>(libusb_get_device_address(devices[index]));

      auto name = std::ostringstream{};
      name << "USB device " << std::hex << std::setfill('0') << std::setw(4) << descriptor.idVendor
           << ":" << std::setw(4) << descriptor.idProduct;

      peers.push_back(PeerInfo{
          .id = id.str(),
          .display_name = name.str(),
          .vendor_id = descriptor.idVendor,
          .product_id = descriptor.idProduct,
          .usb_speed = speed,
          .theoretical_bits_per_second = theoretical_bits_per_second(speed),
          .supports_high_speed = speed == UsbSpeed::HighSpeed || speed == UsbSpeed::SuperSpeed ||
                                 speed == UsbSpeed::SuperSpeedPlus ||
                                 speed == UsbSpeed::Usb4_20Gbps || speed == UsbSpeed::Usb4_40Gbps ||
                                 speed == UsbSpeed::Usb4_80Gbps,
          .supports_super_speed = speed == UsbSpeed::SuperSpeed ||
                                  speed == UsbSpeed::SuperSpeedPlus ||
                                  speed == UsbSpeed::Usb4_20Gbps ||
                                  speed == UsbSpeed::Usb4_40Gbps || speed == UsbSpeed::Usb4_80Gbps,
      });
    }

    libusb_free_device_list(devices, 1);
    return peers;
  }

  void connect(const PeerInfo& /*peer*/, const NegotiatedConfig& /*config*/) override {
    throw std::runtime_error("libusb transport connect is not implemented yet");
  }

  [[nodiscard]] std::size_t send(std::span<const std::byte> /*payload*/) override {
    throw std::runtime_error("libusb transport send is not implemented yet");
  }

  [[nodiscard]] std::vector<std::byte> receive(std::size_t /*max_bytes*/) override {
    throw std::runtime_error("libusb transport receive is not implemented yet");
  }

  [[nodiscard]] std::size_t receive_into(std::span<std::byte> /*buffer*/) override {
    throw std::runtime_error("libusb transport receive is not implemented yet");
  }

  void close() noexcept override {}

private:
  libusb_context* context_{nullptr};
};

} // namespace

std::unique_ptr<Transport> make_libusb_transport() { return std::make_unique<LibusbTransport>(); }

} // namespace hyperlink
