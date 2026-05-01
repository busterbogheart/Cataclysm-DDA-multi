#pragma once
#ifndef CATA_SRC_MP_GAMESTATE_H
#define CATA_SRC_MP_GAMESTATE_H

#include "character_id.h"
#include <string>

namespace cata_mp {

// Called once per game turn from do_turn() — drains the event queue and
// processes connect/disconnect/action events from remote players.
void process_mp_events();

// Returns a JSON string describing the remote player's current position,
// HP, and nearby visible tiles. Sent to the client after each action.
std::string serialize_remote_player_state();

// Returns true if the given character_id belongs to a remote player NPC.
// Used by monmove() to skip AI processing for human-controlled NPCs.
bool is_remote_player( character_id id );

// Drain the server recv queue and apply each state message to the local avatar.
// Called once per game turn from do_turn() when in client mode.
void client_process_incoming();

// Queue an action JSON to be sent to the server on the next tick when moves are
// available. Replaces any previously queued action (latest keypress wins).
void client_queue_action( const std::string &json );

// True when running as a dedicated headless server (--server flag).
// Suppresses avatar input and display-related paths in the game loop.
bool is_server_mode();
void set_server_mode( bool enabled );


} // namespace cata_mp

#endif // CATA_SRC_MP_GAMESTATE_H
