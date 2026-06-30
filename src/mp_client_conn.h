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

// Connect to server and validate version/password immediately.
// Returns true on success (welcome received).  On false, call
// client_connect_error() to get the human-readable rejection reason.
bool client_connect( const std::string &host, uint16_t port,
                     const std::string &name, const std::string &password = "",
                     const std::string &version = "" );
// Error text from the last failed client_connect() call.
std::string client_connect_error();

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

// --- Auto-reconnect ------------------------------------------------------
// When the TCP link drops UNGRACEFULLY (tunnel blip / RTO timeout — no FIN),
// the client tries to silently re-dial the same host (re-PROBE + re-JOIN, no
// char-creation) instead of ending the session.  A transient direct-VPN blip
// then self-heals.  Suppressed on an intentional host goodbye/session_ending.

// True while a reconnect sweep is in progress (for the HUD "reconnecting…").
bool client_is_reconnecting();

// Disable auto-reconnect (call on intentional teardown / host goodbye so a
// quit doesn't trigger a pointless re-dial loop).  Cleared by the next
// successful client_connect().
void client_disable_reconnect();

} // namespace cata_mp

#endif // CATA_SRC_MP_CLIENT_CONN_H
