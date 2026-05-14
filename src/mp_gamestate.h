#pragma once
#ifndef CATA_SRC_MP_GAMESTATE_H
#define CATA_SRC_MP_GAMESTATE_H

#include "activity_type.h"
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

// Enrich a client action JSON with the current client_light and client_bleed fields.
// Call before any direct client_send() to ensure the server always receives light/bleed state.
std::string client_enrich_action( const std::string &json );

// Client only: call immediately after client_send() to suppress stale moves>0
// state packets until the server sends moves=0 (its action acknowledgement).
// Prevents TCP-buffered pre-ack grants from re-unlocking the client.
void client_mark_action_sent();

// Client only: returns true if we have sent an action and are waiting for the
// server's acknowledgement (moves<=0 packet).  Used by do_turn() to block the
// game loop and by mp_dispatch to avoid double-sending while ack is pending.
bool is_client_waiting_for_ack();

// Save the last smash action JSON so it can be re-queued for "keep smashing".
void client_set_autosmash_json( const std::string &json );

// Client only: after any action that consumed avatar moves, send a "wait" to
// the server so the grant/ack cycle advances.  If pre_moves is provided, only
// fires when the avatar has fewer moves now than it did before the action
// (i.e. moves were actually spent).  Pass INT_MAX to fire unconditionally.
// Also resyncs worn items and clears local moves to 0.
void mp_client_post_action( int pre_moves = INT_MAX );

// Ensure the MP debug HUD overlay is active. Safe to call every turn.
void ensure_mp_hud();

// True when running as a dedicated headless server (--server flag).
// Suppresses avatar input and display-related paths in the game loop.
bool is_server_mode();
void set_server_mode( bool enabled );

// True in any multiplayer role: host (--host) or dedicated server (--server).
bool is_mp_mode();

// True when running as an in-process host (--host flag, listen server in background thread).
bool is_host_mode();
void set_host_mode( bool enabled );

// True when this instance is hosting (listen server running), regardless of
// whether it is also a headless dedicated server.
bool is_hosting();

// True when hosting and the host avatar has an active wait activity (ACT_WAIT,
// ACT_WAIT_STAMINA, etc.).  Used by do_turn to skip monmove() so monsters
// freeze while the host fast-forwards, giving the client time to act freely.
bool host_is_in_wait_activity();

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

// Server only: capture avatar-generated messages since pre_msg and queue them
// for the client with "You" → host character name substitution.
// Call from do_turn() after each handle_action() when a remote player is connected.
void host_capture_avatar_msgs( size_t pre_msg );
// Capture ALL messages from vehmove() and queue them for the remote client as
// their own messages (vehicle collision/status messages the driver should see).
void host_capture_vehmove_msgs( size_t pre_msg );

// Write a [cdda-mp] log line to stdout AND to /tmp/cdda-mp-server.log or
// /tmp/cdda-mp-client.log (depending on mode).  Use this for any event that
// should be readable after a session without stopping the process.
void mp_log( const std::string &msg );

// Client only: re-send the client's current worn-item list, skin tone, hair,
// and wielded weapon to the server so the remote NPC proxy stays in sync.
// Call after any wear/take-off/wield action.
void client_resync_worn();

// Client only: if the avatar has an active wait-type activity (ACT_WAIT,
// ACT_WAIT_STAMINA, etc.), dispatch a "wait" action to the server so its
// timeline advances in sync with the client's local activity.  Call once per
// game turn after the avatar activity loop has run and consumed moves.
// Pass the activity ID that was running BEFORE the loop so the dispatch still
// fires when the activity consumed moves and then called finish() this same turn.
void client_dispatch_wait_for_activity( const activity_id &pre_id = activity_id(), bool force_idle = false );

// Client only: returns the luminance emitted by the host player (flashlight,
// mutations, etc.) as received in the last state packet.  Used by lightmap.cpp
// to inject a point light at the host NPC position during build_map_cache().
float get_host_luminance();

// Host only: returns the luminance emitted by the remote player (client), as
// received in each action packet.  Used by lightmap.cpp to inject a point light
// at the remote NPC proxy position during build_map_cache().
float get_remote_player_luminance();

// Client only: returns the character_id of the host NPC proxy so lightmap.cpp
// can find the right NPC without knowing internal MP state.
character_id get_host_npc_character_id();

// Host only: returns the character_id of the remote player NPC proxy so
// lightmap.cpp can inject the client's light at the correct position.
character_id get_remote_player_npc_character_id();

// Client only: milliseconds since the server last granted moves to the client.
// Returns a large value if no grant has been received yet.
// Used by do_turn() to auto-send "wait" when the player is idle and the host
// is fast-forwarding through a long activity (wait, sleep, crafting).
int ms_since_last_grant();

// Client only: true when the server's last state packet indicated the host is
// in a long automatic activity (ACT_WAIT via the | menu, sleep, crafting).
// Used by do_turn() to lower the auto-wait idle threshold from 500ms → 100ms
// so the client responds within the server's 200ms ACT_WAIT tick window.
bool is_host_fenced();

// Client only: true when the server has told us the proxy NPC is at vehicle controls.
// handle_action uses this to route movement keys through pldrive instead of walk.
bool client_ctrl_veh();

// Client only: immediately set the local ctrl-veh flag (without waiting for the next
// state packet).  Use when the client releases controls via the drive menu so that
// movement keys stop routing to pldrive right away.
void set_client_ctrl_veh( bool b );

// Client only: absolute map position of the vehicle the client is currently
// controlling, as broadcast by the server in the most recent state packet.
// Returns a zero tripoint when not driving.
tripoint_abs_ms client_ctrl_veh_abs();

// Host only: called by sdlsound to enqueue an sfx event for forwarding to the
// client in the next grant.  Silently ignored when not hosting.
void host_queue_sfx( const std::string &id, const std::string &variant, int vol );

} // namespace cata_mp

#endif // CATA_SRC_MP_GAMESTATE_H
