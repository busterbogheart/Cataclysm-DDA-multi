#pragma once
#ifndef CATA_SRC_MP_CLIENT_CONN_H
#define CATA_SRC_MP_CLIENT_CONN_H

#include <cstdint>
#include <string>

namespace cata_mp {

bool is_client_mode();
void set_client_mode( bool enabled );

// Bounded TCP reachability check.  Returns true if a TCP connection to
// host:port completes within `timeout_ms` (does NOT join — connection is
// closed immediately).  Use this from the main-menu join path so a typo'd
// IP returns in 2-3 s instead of hanging on macOS's default SYN-retry of
// 75 s.  Safe to call without an active MP session.
bool tcp_probe( const std::string &host, uint16_t port, int timeout_ms );

// Connect to server (TCP only). Returns true on success.
// Does NOT send the join message — call client_send_join() after the save is loaded.
bool client_connect( const std::string &host, uint16_t port,
                     const std::string &name, const std::string &password = "" );

// Send the deferred join message. No-op if already sent or not connected.
// Called by client_process_incoming() on the first game tick.
void client_send_join();

// Returns true after client_send_join() has fired (i.e. the save is loaded and
// the server has received the join handshake).
bool client_join_is_sent();

// Send an action JSON to the server (non-blocking, queued on io thread).
void client_send( const std::string &json );

// Pop one incoming message from the recv queue. Returns false when empty.
// Used by mp_gamestate::client_process_incoming() to apply server state.
bool client_recv_pop( std::string &out );

} // namespace cata_mp

#endif // CATA_SRC_MP_CLIENT_CONN_H
