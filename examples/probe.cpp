#include "hyperlink/session.hpp"

#include <iostream>

int main() {
#if HYPERLINK_HAS_LIBUSB
  auto session = hyperlink::Session{hyperlink::make_libusb_transport()};
  const auto peers = session.discover(hyperlink::AutoConfigOptions{});
  std::cout << "Discovered " << peers.size() << " USB candidate device(s)\n";
  for (const auto& peer : peers) {
    std::cout << "  " << peer.id << " " << peer.display_name << " "
              << hyperlink::to_string(peer.usb_speed);
    if (peer.theoretical_bits_per_second > 0) {
      std::cout << " theoretical=" << (peer.theoretical_bits_per_second / 1'000'000'000.0)
                << "Gbps";
    }
    std::cout << '\n';
  }
#else
  std::cout << "Hyperlink was built without libusb support.\n";
  std::cout << "Install libusb and reconfigure CMake to enable USB probing.\n";
#endif
  return 0;
}
