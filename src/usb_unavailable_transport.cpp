#include "hyperlink/session.hpp"

#include <memory>
#include <stdexcept>

namespace hyperlink {

std::unique_ptr<Transport> make_libusb_transport() {
  throw std::runtime_error("Hyperlink was built without libusb support");
}

} // namespace hyperlink
