void validation_rejects_bad_packet_size();
void negotiation_chooses_conservative_shared_settings();
void negotiation_throws_on_invalid_input();
void session_requires_transport();
void session_discovers_and_transfers_after_connect();
void send_requires_connected_session();
void parses_v2_response_and_uses_source_address();
void rejects_invalid_v2_response();
void chooses_highest_rate_then_stable_tie_breaker();
void probes_every_deduplicated_candidate_and_chooses_fastest();
void reports_all_failed_probes();
void probes_same_host_at_each_distinct_probe_port();
void continues_after_a_timed_out_probe();
void selected_connection_retains_caller_buffer_settings();

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
  probes_every_deduplicated_candidate_and_chooses_fastest();
  reports_all_failed_probes();
  probes_same_host_at_each_distinct_probe_port();
  continues_after_a_timed_out_probe();
  selected_connection_retains_caller_buffer_settings();
  return 0;
}
