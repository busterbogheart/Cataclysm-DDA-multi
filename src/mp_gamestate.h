#pragma once
#ifndef CATA_SRC_MP_GAMESTATE_H
#define CATA_SRC_MP_GAMESTATE_H

#include "character_id.h"
#include "coordinates.h"
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

// Block until the server has sent the initial position and the avatar has been
// teleported to the host area.  Call once after the game world is loaded but
// before the first do_turn(), so the player never sees their scenario start tile.
// Times out after ~5 seconds if no position arrives.
void client_wait_for_initial_position();

// Queue an action JSON to be sent to the server on the next tick when moves are
// available. Replaces any previously queued action (latest keypress wins).
void client_queue_action( const std::string &json );

// Client only: call immediately after client_send() to suppress stale moves>0
// state packets until the server sends moves=0 (its action acknowledgement).
// Prevents TCP-buffered pre-ack grants from re-unlocking the client.
void client_mark_action_sent();

// Ensure the MP debug HUD overlay is active. Safe to call every turn.
void ensure_mp_hud();

// True when running as a dedicated headless server (--server flag).
// Suppresses avatar input and display-related paths in the game loop.
bool is_server_mode();
void set_server_mode( bool enabled );

// True when running as an in-process host (--host flag, listen server in background thread).
bool is_host_mode();
void set_host_mode( bool enabled );

// True when this instance is hosting (listen server running), regardless of
// whether it is also a headless dedicated server.
bool is_hosting();

// Client only: returns true when the client host-NPC proxy occupies the given
// absolute map position.  Used by handle_action to block walk-through-host.
bool is_client_host_at( const tripoint_abs_ms &abs );

// Server only: broadcast a host_died packet to connected clients so they can
// show a "waiting for respawn" message instead of a raw disconnect spam.
// Call this before the death screen takes over.
void notify_client_host_died();

// Server only: grant the remote player one turn's worth of moves and broadcast
// the updated state so the client knows it can act.  Call once per game turn,
// at the top of do_turn(), before the host enters its input loop.
void grant_client_turn();

// Server only: block until the remote player has submitted at least one action
// this turn, then return.  Times out after 30 s so a slow/disconnected client
// never freezes the host permanently.  Call just before monmove().
void wait_for_client_action();

// Server only: record the duration of the last monster/NPC AI turn (in ms) for
// display on the debug HUD.  Call from do_turn() immediately after monmove().
void set_last_monmove_ms( int ms );

// Write a [cdda-mp] log line to stdout AND to /tmp/cdda-mp-server.log or
// /tmp/cdda-mp-client.log (depending on mode).  Use this for any event that
// should be readable after a session without stopping the process.
void mp_log( const std::string &msg );

// Client only: re-send the client's current worn-item list, skin tone, hair,
// and wielded weapon to the server so the remote NPC proxy stays in sync.
// Call after any wear/take-off/wield action.
void client_resync_worn();

} // namespace cata_mp

#endif // CATA_SRC_MP_GAMESTATE_H
