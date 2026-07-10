void validation_rejects_bad_packet_size();
void negotiation_chooses_conservative_shared_settings();
void negotiation_throws_on_invalid_input();
void session_requires_transport();
void session_discovers_and_transfers_after_connect();
void send_requires_connected_session();
void parses_v2_response_and_uses_source_address();
void rejects_invalid_v2_response();
void chooses_highest_rate_then_stable_tie_breaker();

int main() {
  validation_rejects_bad_packet_size();
  negotiation_chooses_conservative_shared_settings();
  negotiation_throws_on_invalid_input();
  session_requires_transport();
  session_discovers_and_transfers_after_connect();
  send_requires_connected_session();
  parses_v2_response_and_uses_source_address();
  rejects_invalid_v2_response();
  chooses_highest_rate_then_stable_tie_breaker();
  return 0;
}
