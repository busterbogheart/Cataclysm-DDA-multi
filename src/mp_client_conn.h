#pragma once
#ifndef CATA_SRC_MP_CLIENT_CONN_H
#define CATA_SRC_MP_CLIENT_CONN_H

#include <cstdint>
#include <string>

namespace cata_mp {

bool is_client_mode();
void set_client_mode( bool enabled );

// Connect to server (TCP only). Returns true on success.
// Does NOT send the join message — call client_send_join() after the save is loaded.
bool client_connect( const std::string &host, uint16_t port,
                     const std::string &name, const std::string &password = "" );

// Send the deferred join message. No-op if already sent or not connected.
// Called by client_process_incoming() on the first game tick.
void client_send_join();

// Send an action JSON to the server (non-blocking, queued on io thread).
void client_send( const std::string &json );

// Pop one incoming message from the recv queue. Returns false when empty.
// Used by mp_gamestate::client_process_incoming() to apply server state.
bool client_recv_pop( std::string &out );

} // namespace cata_mp

#endif // CATA_SRC_MP_CLIENT_CONN_H
