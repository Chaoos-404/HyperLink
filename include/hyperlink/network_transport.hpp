#pragma once

#include "hyperlink/export.hpp"
#include "hyperlink/session.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hyperlink {

struct HYPERLINK_API TcpEndpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{47777};
  std::uint32_t send_buffer_bytes{0};
  std::uint32_t receive_buffer_bytes{0};
};

[[nodiscard]] HYPERLINK_API std::unique_ptr<Transport>
make_tcp_client_transport(TcpEndpoint endpoint);

[[nodiscard]] HYPERLINK_API std::unique_ptr<Transport>
make_tcp_server_transport(TcpEndpoint bind_endpoint);

} // namespace hyperlink
