void validation_rejects_bad_packet_size();
void negotiation_chooses_conservative_shared_settings();
void negotiation_throws_on_invalid_input();
void session_requires_transport();
void session_discovers_and_transfers_after_connect();
void send_requires_connected_session();

int main() {
  validation_rejects_bad_packet_size();
  negotiation_chooses_conservative_shared_settings();
  negotiation_throws_on_invalid_input();
  session_requires_transport();
  session_discovers_and_transfers_after_connect();
  send_requires_connected_session();
  return 0;
}
