#pragma once
#ifndef CATA_SRC_MP_GAMESTATE_H
#define CATA_SRC_MP_GAMESTATE_H

#include <string>

namespace cata_mp {

// Called once per game turn from do_turn() — drains the event queue and
// processes connect/disconnect/action events from remote players.
void process_mp_events();

// Returns a JSON string describing the remote player's current position,
// HP, and nearby visible tiles. Sent to the client after each action.
std::string serialize_remote_player_state();

// True when running as a dedicated headless server (--server flag).
// Suppresses avatar input and display-related paths in the game loop.
bool is_server_mode();
void set_server_mode( bool enabled );

} // namespace cata_mp

#endif // CATA_SRC_MP_GAMESTATE_H
