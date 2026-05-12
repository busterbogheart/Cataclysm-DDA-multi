#include "mp_gamestate.h"
#include "mp_client_conn.h"
#include "input.h"
#include "mp_queue.h"
#include "mp_server.h"

#include "avatar.h"
#include "calendar.h"
#include "cata_utility.h"
#include "character_id.h"
#include "color.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "cursesdef.h"
#include "game.h"
#include "gates.h"
#include "item.h"
#include "itype.h"
#include "json.h"
#include "json_loader.h"
#include "monster.h"
#include "mtype.h"
#include "field.h"
#include "field_type.h"
#include "map.h"
#include "map_scale_constants.h"
#include "move_mode.h"
#include "memory_fast.h"
#include "messages.h"
#include "npc.h"
#include "output.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "point.h"
#include "filesystem.h"
#include "effect.h"
#include "mutation.h"
#include "teleport.h"
#include "activity_type.h"
#include "player_activity.h"
#include "trap.h"
#include "type_id.h"
#include "ui_manager.h"
#include "rng.h"
#include "units.h"
#include "units_utility.h"
#include "skill.h"
#include "vehicle.h"
#ifdef TILES
#include "sdl_wrappers.h"
#include "sounds.h"
#endif
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cata_mp {

void mp_log( const std::string &msg )
{
    std::cout << msg << std::endl;
    static std::ofstream logfile;
    if( !logfile.is_open() ) {
        const std::string path = is_client_mode()
                                 ? "/tmp/cdda-mp-client.log"
                                 : "/tmp/cdda-mp-server.log";
        logfile.open( path, std::ios::app );
    }
    if( logfile.is_open() ) {
        logfile << msg << '\n';
        logfile.flush();
    }
}

static bool server_mode_ = false;

bool is_server_mode()
{
    return server_mode_;
}

void set_server_mode( bool enabled )
{
    server_mode_ = enabled;
}

static bool host_mode_ = false;

bool is_host_mode()
{
    return host_mode_;
}

void set_host_mode( bool enabled )
{
    host_mode_ = enabled;
}

bool is_mp_mode()
{
    return server_mode_ || host_mode_;
}

bool is_hosting()
{
    return get_active_server() != nullptr;
}

// Host: sfx events generated during this turn, forwarded to the client in the next grant.
// Capped at 32 entries per turn to bound grant message size.
struct MPSfxEvent {
    std::string id;
    std::string variant;
    int vol = 0;
};
static std::vector<MPSfxEvent> g_host_sfx_queue;

void host_queue_sfx( const std::string &id, const std::string &variant, int vol )
{
    if( !is_hosting() || vol < 5 ) {
        return;
    }
    if( g_host_sfx_queue.size() >= 32 ) {
        return;
    }
    g_host_sfx_queue.push_back( {id, variant, vol} );
}

// The character_id of the remote player's NPC. Invalid when no remote player is connected.
static character_id remote_player_npc_id;
static bool remote_player_connected = false;
static std::string remote_player_name_;

// Client-side: NPC representing the host player in the client's local world.
static character_id client_host_npc_id;
static bool client_host_npc_spawned = false;

// Server: monotonically increasing counter for assigning monster/vehicle network IDs.
static uint32_t g_next_net_id = 0;

// Server: maps vehicle pointers to stable network IDs for vehicle position sync.
static std::unordered_map<vehicle *, uint32_t> g_server_veh_ids;

// Client: maps server vehicle network IDs to the last-known absolute tile position.
// Used to look up the vehicle object before moving it to the server-authoritative position.
static std::unordered_map<uint32_t, tripoint_abs_ms> g_client_veh_pos;

// Server: cumulative AP for the remote player (replaces the NPC's own moves which
// are skipped by monmove since is_remote_player() returns true).
static int g_remote_moves = 0;

// Server: true while wait_for_client_action() is blocking.  Used by the HUD
// strip so it shows red (can't act) even when host moves > 0.
static bool g_host_waiting_for_client = false;

// Server: message log index at last state broadcast — used to forward only NEW messages.
static size_t g_last_forwarded_msg_count = 0;

// Server: messages captured during a remote player action that are forwarded
// verbatim (after NPC→"You" substitution) regardless of NPC name filter.
// Cleared by serialize_remote_player_state() when it consumes them.
static std::vector<std::string> g_action_msgs_pending;

// Server: messages from the avatar's own actions, forwarded to the client with
// "You" → host character name so the client sees attributed host hits.
// Populated by host_capture_avatar_msgs(); cleared by serialize_remote_player_state().
static std::vector<std::string> g_host_action_msgs_pending;

// Client: maps server-assigned network IDs to local monster pointers.
// Rebuilt each sync tick from creature_tracker before applying updates.
static std::unordered_map<uint32_t, monster *> g_net_id_map;

// Client: action JSON queued to auto-fire once the server grants moves again.
// Latest keypress wins — pressing a different key replaces the queued action.
static std::string g_pending_action;

// Client: "keep smashing" continuation state.
// When set, auto-re-queue the smash action after each partial bash until the
// target is destroyed or the bash fails to make progress.
static bool g_client_autosmash = false;
static std::string g_client_autosmash_json; // the smash JSON to re-queue

// Client: set when the host sends host_died or the socket drops.
// Suppresses further processing and the lost-connection spam.
static bool g_server_died = false;

// Client: set after sending an action, cleared when the server sends moves=0.
// While true, incoming state packets with moves>0 are stale pre-ack broadcasts
// and must be ignored — otherwise TCP-buffered grants re-unlock the client
// before the server has processed the action.
static bool g_client_waiting_for_ack = false;
// Client: true when the server's proxy NPC is at vehicle controls.
// Set by apply_state_packet() from the "client_ctrl_veh" field.
// handle_action uses this to route movement keys to pldrive.
static bool g_client_ctrl_veh = false;
// Client: absolute position of the controlled vehicle (root tile).
// Set alongside g_client_ctrl_veh from "client_veh_pos" in the state packet.
static tripoint_abs_ms g_client_ctrl_veh_abs{ 0, 0, 0 };
// Timestamp of when the ack guard was set. Used to break deadlocks where the
// server never sends moves<=0 (e.g. after reconnect with a stale ack flag).
static std::chrono::steady_clock::time_point g_ack_set_time;
// Timestamp of when the server last granted moves (moves > 0 packet received).
// Used by ms_since_last_grant() to auto-send "wait" when the player is idle.
static std::chrono::steady_clock::time_point g_last_grant_time;

// Server: set when the remote player has submitted at least one real action
// this turn.  Cleared by grant_client_turn(); checked by wait_for_client_action().
static bool g_client_acted_this_turn = false;

// Server: elapsed wait time (ms) in the last wait_for_client_action() call.
static int g_wait_elapsed_ms = 0;
// Server: duration (ms) of the last monmove() (AI turn) call; set by do_turn.cpp.
static int g_last_monmove_ms = 0;

// Server: short label for the last action type received from the client this turn.
// Reset to em-dash by grant_client_turn() at the start of each host turn.
// Displayed on the server HUD "Queued" row as the partner-side equivalent.
static std::string g_last_client_action_label = "\xe2\x80\x94";

// Separation warning tier: 0=ok, 1=warn (≥50 tiles), 2=danger (≥57 tiles).
// Shared by both host and client; resets on connect/disconnect.
// Hysteresis: step up at 50/57, step down at 44/50.
static int g_separation_tier = 0;

// Client: luminance emitted by the host player (flashlight, mutations, etc.).
// Received from state packet each turn and injected into the lighting pass.
static float g_mp_host_luminance = 0.0f;

// Host: luminance emitted by the remote player (client). Received from each
// action packet and injected into the host's lighting pass at the proxy NPC position.
static float g_mp_remote_player_luminance = 0.0f;

static const efftype_id effect_bleed( "bleed" );

// ---------------------------------------------------------------------------
// MP debug HUD
// ---------------------------------------------------------------------------

static std::string pending_label()
{
    if( g_pending_action.empty() ) {
        return "\xe2\x80\x94"; // em dash
    }
    if( g_pending_action.find( "\"action\":\"move\"" ) != std::string::npos ) {
        for( const char *d : { "ne", "nw", "se", "sw", "n", "s", "e", "w" } ) {
            if( g_pending_action.find( std::string( "\"dir\":\"" ) + d + "\"" ) != std::string::npos ) {
                return std::string( "move:" ) + d;
            }
        }
        return "move";
    }
    if( g_pending_action.find( "\"action\":\"pickup\"" ) != std::string::npos ) {
        return "pickup";
    }
    if( g_pending_action.find( "\"action\":\"wait\"" ) != std::string::npos ) {
        return "wait";
    }
    if( g_pending_action.find( "\"action\":\"smash\"" ) != std::string::npos ) {
        return "smash";
    }
    if( g_pending_action.find( "\"action\":\"open\"" ) != std::string::npos ) {
        return "open";
    }
    if( g_pending_action.find( "\"action\":\"close\"" ) != std::string::npos ) {
        return "close";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Info panel (bottom-left corner)
// ---------------------------------------------------------------------------

struct mp_hud_t {
    catacurses::window win;
    ui_adaptor ui;

    static constexpr int W = 46;
    static constexpr int H = 4;

    mp_hud_t() {
        ui.on_screen_resize( [this]( ui_adaptor &ua ) {
            win = catacurses::newwin( H, W, point( 0, TERMY - H ) );
            ua.position_from_window( win );
        } );
        ui.on_redraw( [this]( const ui_adaptor & ) {
            draw();
        } );
        ui.mark_resize();
    }

    void draw() const {
        werase( win );
        draw_border( win );

        mvwprintz( win, point( ( W - 8 ) / 2, 0 ), c_cyan, " Co-op " );

        if( is_client_mode() ) {
            // Row 1: my queued action
            const std::string pend = pending_label();
            mvwprintz( win, point( 2, 1 ), c_white, "Queued: " );
            mvwprintz( win, point( 10, 1 ),
                       pend == "\xe2\x80\x94" ? c_dark_gray : c_yellow, pend );

            // Row 2: host status (from client's perspective)
            const bool my_turn = get_avatar().get_moves() > 0;
            mvwprintz( win, point( 2, 2 ), c_white, "Host:   " );
            mvwprintz( win, point( 10, 2 ),
                       my_turn ? c_dark_gray : c_light_green,
                       my_turn ? "waiting for you" : "acting..." );

        } else {
            // Row 1: partner's last action
            mvwprintz( win, point( 2, 1 ), c_white, "Queued: " );
            mvwprintz( win, point( 10, 1 ),
                       g_last_client_action_label == "\xe2\x80\x94" ? c_dark_gray : c_yellow,
                       g_last_client_action_label );

            // Row 2: partner connection status
            std::string plabel = remote_player_name_.empty() ? "Partner" : remote_player_name_;
            if( plabel.size() > 10 ) {
                plabel = plabel.substr( 0, 9 ) + "~";
            }
            plabel += ":";
            mvwprintz( win, point( 2, 2 ), c_white, "%-12s", plabel.c_str() );
            if( remote_player_connected ) {
                const bool acted = g_client_acted_this_turn;
                mvwprintz( win, point( 14, 2 ),
                           acted ? c_light_green : c_yellow,
                           acted ? "ready" : "acting..." );
            } else {
                mvwprintz( win, point( 14, 2 ), c_dark_gray, "not connected" );
            }
        }

        wnoutrefresh( win );
    }
};

// ---------------------------------------------------------------------------
// Full-height go/stop strip on the left edge (both host and client)
// ---------------------------------------------------------------------------

struct mp_strip_t {
    catacurses::window win;
    ui_adaptor ui;

    mp_strip_t() {
        ui.on_screen_resize( [this]( ui_adaptor &ua ) {
            win = catacurses::newwin( TERMY, 1, point( 0, 0 ) );
            ua.position_from_window( win );
        } );
        ui.on_redraw( [this]( const ui_adaptor & ) {
            draw();
        } );
        ui.mark_resize();
    }

    void draw() const {
        werase( win );
        // Show locked (red) when moves ≤ 0 OR when a wait activity owns the move
        // budget (e.g. catching breath after sprinting).  Raw moves > 0 while a wait
        // activity is running is misleading — the player can't actually take actions.
        const player_activity &pact = get_avatar().activity;
        static const activity_id s_act_wait( "ACT_WAIT" );
        static const activity_id s_act_wait_stamina( "ACT_WAIT_STAMINA" );
        static const activity_id s_act_wait_weather( "ACT_WAIT_WEATHER" );
        static const activity_id s_act_wait_npc( "ACT_WAIT_NPC" );
        const bool in_wait_act = pact && (
            pact.id() == s_act_wait || pact.id() == s_act_wait_stamina ||
            pact.id() == s_act_wait_weather || pact.id() == s_act_wait_npc );
        const bool go = get_avatar().get_moves() > 0 && !in_wait_act
                        && !g_host_waiting_for_client;
        const nc_color c = go ? c_light_green : c_red;
        for( int y = 0; y < TERMY; y++ ) {
            mvwprintz( win, point( 0, y ), c, "\xe2\x96\x88" );
        }
        wnoutrefresh( win );
    }
};

static std::unique_ptr<mp_strip_t> g_mp_strip;
static std::unique_ptr<mp_hud_t> g_mp_hud;

void ensure_mp_hud()
{
    // Strip rendered first so panel draws on top in the overlap zone.
    if( !g_mp_strip ) {
        g_mp_strip = std::make_unique<mp_strip_t>();
    }
    g_mp_strip->ui.invalidate_ui();
    if( !g_mp_hud ) {
        g_mp_hud = std::make_unique<mp_hud_t>();
    }
    g_mp_hud->ui.invalidate_ui();
}

// Client: last confirmed position of the remote player (our avatar as seen by the server).
// Used as the center of the monster sync region.
static tripoint_abs_ms g_mp_remote_pos{ 0, 0, 0 };

// Client: true after the first successful long-range teleport to the host area.
// The heavy place_player_overmap() is only needed once per session.
static bool g_initial_teleport_done = false;

// Server: per-tile baseline (ter + furn + item fingerprint) for dirty-tile tracking.
struct mp_tile_state {
    std::string ter;
    std::string furn;
    std::string items_sig;    // "type:charges,..." — empty when no items
    std::string fields_sig;   // "type:intensity,..." — empty when no fields
    std::string trap_sig;     // trap id string, empty = tr_null (no placed trap)
    std::string graffiti_sig; // empty = no graffiti
};
static std::unordered_map<tripoint_abs_ms, mp_tile_state> g_tile_baseline;

// Client→server tile baselines: track what was last sent so we only send diffs.
// Fields are always re-sent every turn because they decay server-side.
static std::unordered_map<tripoint_abs_ms, std::string> g_client_item_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_terfurn_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_trap_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_graffiti_baseline;

static std::string json_escape_str( const std::string &s )
{
    std::string out;
    out.reserve( s.size() );
    for( char c : s ) {
        if( c == '"' )       { out += "\\\""; }
        else if( c == '\\' ) { out += "\\\\"; }
        else if( c == '\n' ) { out += "\\n";  }
        else if( c == '\r' ) { out += "\\r";  }
        else if( c == '\t' ) { out += "\\t";  }
        else                 { out += c;       }
    }
    return out;
}

// Client: last known HP per net ID — used to synthesise combat hit/death messages.
static std::unordered_map<uint32_t, int> g_last_monster_hp;

// Client: last known HP per bodypart string ID — used to synthesise "you were hit" messages.
static std::unordered_map<std::string, int> g_last_bodypart_hp;

// Client: fingerprint of the last host_worn list applied — avoids re-dressing every tick.
static std::string g_client_host_worn_sig;

// ---------------------------------------------------------------------------
// Stale MP NPC cleanup — cross-session
// ---------------------------------------------------------------------------

// Path of the list of MP NPC IDs that were alive when the last session ended.
// Any NPC whose ID is in this file should be removed at startup.
static cata_path mp_npc_cleanup_path()
{
    return PATH_INFO::world_base_save_path() / "mp_npc_cleanup.json";
}

// Write the IDs of all currently-live MP NPCs so the next session can purge them.
// Called whenever we spawn or despawn MP NPCs.
static void mp_save_npc_ids()
{
    std::vector<int> ids;
    std::vector<std::string> names;
    if( remote_player_npc_id.is_valid() ) {
        ids.push_back( remote_player_npc_id.get_value() );
        npc *rn = g->critter_by_id<npc>( remote_player_npc_id );
        if( rn && !rn->name.empty() ) {
            names.push_back( rn->name );
        }
    }
    if( client_host_npc_id.is_valid() ) {
        ids.push_back( client_host_npc_id.get_value() );
    }
    if( ids.empty() && names.empty() ) {
        remove_file( mp_npc_cleanup_path() );
        return;
    }
    write_to_file( mp_npc_cleanup_path(), [&]( std::ostream &fout ) {
        JsonOut jo( fout );
        jo.start_object();
        jo.member( "ids" );
        jo.start_array();
        for( int id : ids ) {
            jo.write( id );
        }
        jo.end_array();
        jo.member( "names" );
        jo.start_array();
        for( const std::string &nm : names ) {
            jo.write( nm );
        }
        jo.end_array();
        jo.end_object();
    }, "mp npc cleanup list" );
}

static void purge_npcs_by_name( const std::string &name );

// Remove stale MP NPCs saved from a previous session.
// Called once per process lifetime, early in the game loop.
static bool mp_cleanup_done = false;
static void mp_cleanup_stale_npcs()
{
    if( mp_cleanup_done ) {
        return;
    }
    mp_cleanup_done = true;

    const cata_path path = mp_npc_cleanup_path();
    bool found_any = false;
    read_from_file_optional_json( path, [&]( const JsonValue &jv ) {
        // Support both the old array format (ids only) and the new object format
        // (ids + names).  The old format was written by earlier versions of the
        // code and may still be present in existing saves.
        if( jv.test_array() ) {
            // Legacy: plain array of integer IDs
            for( const JsonValue &v : jv.get_array() ) {
                const character_id id( v.get_int() );
                found_any = true;
                npc *n = g->critter_by_id<npc>( id );
                if( n ) {
                    n->die( &get_map(), nullptr );
                }
                if( id.is_valid() ) {
                    overmap_buffer.remove_npc( id );
                }
            }
        } else {
            JsonObject jo = jv.get_object();
            for( const JsonValue &v : jo.get_array( "ids" ) ) {
                const character_id id( v.get_int() );
                found_any = true;
                npc *n = g->critter_by_id<npc>( id );
                if( n ) {
                    n->die( &get_map(), nullptr );
                }
                if( id.is_valid() ) {
                    overmap_buffer.remove_npc( id );
                }
            }
            for( const JsonValue &v : jo.get_array( "names" ) ) {
                const std::string nm = v.get_string();
                found_any = true;
                purge_npcs_by_name( nm );
            }
        }
    } );
    if( found_any ) {
        g->cleanup_dead();
        std::cout << "[cdda-mp] Cleaned up stale MP NPCs from previous session." << std::endl;
    }
    remove_file( path );
}

// Remove every NPC with the given name from both the active critter list and the
// overmap buffer. Eliminates artifacts left by previous sessions before the
// ID-tracking cleanup mechanism existed.
static void purge_npcs_by_name( const std::string &name )
{
    if( name.empty() ) {
        return;
    }
    // Active critters first
    for( npc *n : g->get_npcs_if( [&]( const npc &np ) { return np.name == name; } ) ) {
        n->die( &get_map(), nullptr );
    }
    g->cleanup_dead();
    // Overmap buffer — radius 500 sm covers the entire typical play area
    for( const auto &ptr : overmap_buffer.get_npcs_near_player( 500 ) ) {
        if( ptr && ptr->name == name && ptr->getID().is_valid() ) {
            overmap_buffer.remove_npc( ptr->getID() );
        }
    }
}

static cata_path remote_player_save_path( const std::string &name )
{
    std::string safe;
    for( char c : name ) {
        safe += std::isalnum( static_cast<unsigned char>( c ) ) ? c : '_';
    }
    return PATH_INFO::world_base_save_path() / ( "mp_player_" + safe + ".json" );
}

static void save_remote_player()
{
    if( !remote_player_connected ) {
        return;
    }
    npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
    if( !remote ) {
        return;
    }
    const cata_path path = remote_player_save_path( remote->name );
    write_to_file( path, [&]( std::ostream &fout ) {
        JsonOut json( fout );
        remote->serialize( json );
    }, "multiplayer character" );
    std::cout << "[cdda-mp] Saved remote player '" << remote->name << "'" << std::endl;
}

static void spawn_remote_player( const std::string &name )
{
    if( remote_player_connected ) {
        std::cout << "[cdda-mp] Remote player already exists, ignoring spawn." << std::endl;
        return;
    }
    // Purge any stale NPCs from previous sessions that share this name —
    // covers artifacts that predate the ID-tracking cleanup file.
    purge_npcs_by_name( name );

    avatar &u = get_avatar();
    map &m = get_map();

    // Find nearest passable tile that is NOT the host's own tile.
    // Search outward from radius 1; expand up to 20 tiles if needed.
    tripoint_bub_ms spawn_pos = u.pos_bub();
    bool found = false;
    for( int radius = 1; radius <= 20 && !found; ++radius ) {
        for( int dy = -radius; dy <= radius && !found; ++dy ) {
            for( int dx = -radius; dx <= radius && !found; ++dx ) {
                if( std::abs( dx ) != radius && std::abs( dy ) != radius ) {
                    continue; // only check the ring edge
                }
                tripoint_bub_ms candidate = u.pos_bub() + tripoint( dx, dy, 0 );
                if( candidate == u.pos_bub() ) {
                    continue; // never land on the host's exact tile
                }
                if( m.inbounds( candidate ) && !m.impassable( candidate ) ) {
                    spawn_pos = candidate;
                    found = true;
                }
            }
        }
    }
    if( !found ) {
        std::cout << "[cdda-mp] WARNING: no passable spawn tile found near host, using host tile" << std::endl;
    }

    shared_ptr_fast<npc> remote = make_shared_fast<npc>();

    const cata_path save_path = remote_player_save_path( name );
    bool loaded = read_from_file_optional_json( save_path, [&]( const JsonValue &jv ) {
        remote->deserialize( jv.get_object() );
    } );

    if( loaded ) {
        remote->name = name;
        std::cout << "[cdda-mp] Loaded saved character for '" << name << "'" << std::endl;
    } else {
        remote->normalize();
        remote->name = name;
    }

    // Ensure the NPC has a valid character_id before inserting into the world.
    // make_shared_fast<npc>() + normalize() never calls setID(), so we must
    // assign one explicitly here. Loaded NPCs may already have a valid id from
    // their save file; assign_npc_id() is safe to call regardless.
    if( !remote->getID().is_valid() ) {
        remote->setID( g->assign_npc_id() );
    }

    // Always respawn near the host player regardless of saved position
    remote->spawn_at_precise( m.get_abs( spawn_pos ) );
    overmap_buffer.insert_npc( remote );
    g->load_npcs();

    remote_player_npc_id = remote->getID();
    mp_log( "[cdda-mp] spawn: remote_player_npc_id=" +
            std::to_string( remote_player_npc_id.get_value() ) +
            " valid=" + std::to_string( remote_player_npc_id.is_valid() ) );

    // Co-op partner: apply ally status AFTER load_npcs so the NPC pointer is
    // fully wired into the game state (faction manager, critter tracker, etc.).
    npc *rn = g->critter_by_id<npc>( remote_player_npc_id );
    if( rn ) {
        rn->set_attitude( NPCATT_FOLLOW );
        rn->set_fac( faction_id( "your_followers" ) );
        rn->chatbin.first_topic = "TALK_FRIEND";
        rn->op_of_u.trust = 10;
        rn->op_of_u.value = 10;
        g->add_npc_follower( rn->getID() );
    }
    remote_player_connected = true;
    g_remote_moves = rn ? rn->get_speed() : 100;  // grant first turn immediately
    g_client_acted_this_turn = false;
    g_tile_baseline.clear();  // force full resync — client reloads from disk on connect
    g_separation_tier = 0;
    g_last_forwarded_msg_count = Messages::size();  // don't forward pre-connect history
    mp_save_npc_ids();  // persist ID so next session can clean it up

    add_msg( m_good, "%s has connected and joined the game.", name );
    std::cout << "[cdda-mp] Spawned remote player '" << name << "' at "
              << spawn_pos.x() << "," << spawn_pos.y() << std::endl;

    // Send initial state to player 2, then ask it to resend worn/hair so the
    // fresh NPC gets the correct appearance even after a respawn.
    server *srv = get_active_server();
    if( srv ) {
        srv->post_broadcast( serialize_remote_player_state() + "\n" );
        srv->post_broadcast( "{\"type\":\"resync_request\"}\n" );
    }
}

static void remove_remote_player()
{
    if( !remote_player_connected ) {
        return;
    }

    save_remote_player();

    npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
    if( remote ) {
        remote->set_moves( 0 );
        remote->die( &get_map(), nullptr );
        g->cleanup_dead();
    }
    // Remove from overmap buffer so the dead NPC doesn't get written back into
    // the world save, which would cause a stale NPC to appear on next load.
    if( remote_player_npc_id.is_valid() ) {
        overmap_buffer.remove_npc( remote_player_npc_id );
    }

    remote_player_connected = false;
    remote_player_npc_id = character_id();
    g_separation_tier = 0;
    mp_save_npc_ids();  // ID is now invalid — clears the cleanup file entry
    add_msg( m_bad, "The other player has disconnected." );
    std::cout << "[cdda-mp] Remote player removed from world." << std::endl;
}

// After substituting an NPC name → "You", the verb is still third-person singular.
// Strip the suffix so "You guts" → "You gut", "You misses" → "You miss", etc.
// Also fixes "You's " → "your " for possessive constructions.
static void fix_you_verb( std::string &s )
{
    for( size_t p = 0; ( p = s.find( "You's ", p ) ) != std::string::npos; ) {
        s.replace( p, 6, "your " );
    }

    if( s.rfind( "You ", 0 ) != 0 ) {
        return;
    }

    const size_t vs = 4;
    size_t ve = s.find( ' ', vs );
    if( ve == std::string::npos ) {
        ve = s.size();
    }
    if( ve <= vs + 1 ) {
        return;
    }

    const std::string v = s.substr( vs, ve - vs );
    std::string fixed;

    auto ends_with = [&]( const char *suffix, size_t n ) {
        return v.size() >= n && v.compare( v.size() - n, n, suffix ) == 0;
    };

    if( ends_with( "ies", 3 ) ) {
        fixed = v.substr( 0, v.size() - 3 ) + "y";
    } else if( ends_with( "sses", 4 ) || ends_with( "xes", 3 ) || ends_with( "zes", 3 ) ||
               ends_with( "ches", 4 ) || ends_with( "shes", 4 ) ) {
        fixed = v.substr( 0, v.size() - 2 );
    } else if( v.size() > 1 && v.back() == 's' ) {
        fixed = v.substr( 0, v.size() - 1 );
    } else {
        return;
    }

    s.replace( vs, ve - vs, fixed );
}

// Collect messages generated from pre_msg onward, apply NPC→"You" substitution,
// and append them to g_action_msgs_pending for inclusion in the next broadcast.
// Messages starting with "You " that don't contain the NPC name are host-avatar
// messages that have no meaning on the client — they are skipped.
static void flush_action_msgs( size_t pre_msg, const std::string &npc_name )
{
    const size_t cur = Messages::size();
    if( cur <= pre_msg ) {
        return;
    }
    const size_t count = cur - pre_msg;
    g_last_forwarded_msg_count = cur;  // advance watermark to avoid double-forward
    const auto new_msgs = Messages::recent_messages( count );
    for( const auto &[time_str, text] : new_msgs ) {
        ( void )time_str;
        // Substitute NPC name → "You" so client sees first-person messages.
        // Do NOT filter "You ..." messages here — bash/smash/interact success
        // messages are generated by map code without NPC name and must reach
        // the client.  Between-action messages (in serialize_remote_player_state)
        // are NPC-name-filtered separately.
        std::string out = text;
        if( !npc_name.empty() ) {
            // Resolve <npcname> placeholder used by melee/combat message generators
            // (e.g. player_hit_message stores "<npcname> hits X" not the real name).
            // Expand it to the real name first so the name→"You" pass below catches it.
            size_t q = 0;
            while( ( q = out.find( "<npcname>", q ) ) != std::string::npos ) {
                out.replace( q, 9, npc_name );
                q += npc_name.size();
            }
            bool did_sub = false;
            size_t p = 0;
            while( ( p = out.find( npc_name, p ) ) != std::string::npos ) {
                out.replace( p, npc_name.size(), "You" );
                p += 3;
                did_sub = true;
            }
            if( did_sub ) {
                fix_you_verb( out );
            }
        }
        mp_log( "[cdda-mp] flush_action_msgs queued: " + out );
        g_action_msgs_pending.push_back( out );
    }
}

// Capture messages the avatar generated since pre_msg and queue them for the client,
// replacing "You" with the host character's name so the client sees attributed hits.
// Called from do_turn after each avatar handle_action() when a remote player is connected.
// Capture ALL messages generated during vehmove() and queue them for the remote
// client as their own messages (no attribution — the driver sees these directly).
void host_capture_vehmove_msgs( size_t pre_msg )
{
    if( !is_hosting() || !remote_player_connected ) {
        return;
    }
    const size_t cur = Messages::size();
    if( cur <= pre_msg ) {
        return;
    }
    const auto new_msgs = Messages::recent_messages( cur - pre_msg );
    for( const auto &[time_str, text] : new_msgs ) {
        ( void )time_str;
        g_action_msgs_pending.push_back( text );
    }
}

void host_capture_avatar_msgs( size_t pre_msg )
{
    if( !is_hosting() || !remote_player_connected ) {
        return;
    }
    const size_t cur = Messages::size();
    if( cur <= pre_msg ) {
        return;
    }
    const std::string host_name = get_avatar().name;
    const auto new_msgs = Messages::recent_messages( cur - pre_msg );
    for( const auto &[time_str, text] : new_msgs ) {
        ( void )time_str;
        // Only forward messages that look like avatar combat ("You " prefix).
        // Inventory, UI, and ambient messages are excluded.
        if( text.rfind( "You ", 0 ) != 0 ) {
            continue;
        }
        std::string out = text;
        // Replace "You" with the host's character name.
        out.replace( 0, 3, host_name );
        mp_log( "[cdda-mp] host_combat_msg: " + out );
        g_host_action_msgs_pending.push_back( out );
    }
}

static void handle_remote_action( const std::string &/*name*/, const std::string &msg )
{
    if( !remote_player_connected ) {
        return;
    }

    npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
    if( !remote ) {
        return;
    }

    map &m = get_map();

    // Snapshot message count before processing so we can forward ALL messages
    // generated during this action (hits, damage, kills, sounds) to the client.
    const size_t pre_action_msg = Messages::size();

    // Give the NPC its current move budget before executing any action.
    // (monmove skips remote player NPCs, so we manage AP ourselves.)
    remote->set_moves( g_remote_moves );

    // Parse the full action JSON once here so char_stats and other top-level
    // fields are accessible without re-parsing inside each action block.
    JsonValue jv_top = json_loader::from_string( msg );
    JsonObject jo = jv_top.get_object();
    jo.allow_omitted_members();

    // Apply the client's real character stats to the proxy so all action
    // handlers (pldrive steering, melee, etc.) use the player's actual values.
    if( jo.has_object( "char_stats" ) ) {
        JsonObject cs = jo.get_object( "char_stats" );
        cs.allow_omitted_members();
        if( cs.has_int( "str" ) ) {
            remote->set_str_base( cs.get_int( "str" ) );
        }
        if( cs.has_int( "dex" ) ) {
            remote->set_dex_base( cs.get_int( "dex" ) );
        }
        if( cs.has_int( "int" ) ) {
            remote->set_int_base( cs.get_int( "int" ) );
        }
        if( cs.has_int( "per" ) ) {
            remote->set_per_base( cs.get_int( "per" ) );
        }
        if( cs.has_array( "skills" ) ) {
            for( const JsonValue &entry : cs.get_array( "skills" ) ) {
                JsonArray ja = entry.get_array();
                const skill_id sid( ja.next_string() );
                const int lvl = ja.next_int();
                remote->set_skill_level( sid, lvl );
            }
        }
        if( cs.has_array( "profs" ) ) {
            for( const JsonValue &pv : cs.get_array( "profs" ) ) {
                const proficiency_id pid( pv.get_string() );
                if( !remote->has_proficiency( pid ) ) {
                    remote->add_proficiency( pid, true );
                }
            }
        }
    }

    // Extract action label for HUD "Queued" row on the server panel.
    {
        const size_t pos = msg.find( "\"action\":\"" );
        if( pos != std::string::npos ) {
            const size_t start = pos + 10;
            const size_t end = msg.find( '"', start );
            if( end != std::string::npos ) {
                std::string act = msg.substr( start, end - start );
                // Append direction for move actions so it reads e.g. "move:n".
                if( act == "move" ) {
                    const size_t dpos = msg.find( "\"dir\":\"" );
                    if( dpos != std::string::npos ) {
                        const size_t dstart = dpos + 7;
                        const size_t dend = msg.find( '"', dstart );
                        if( dend != std::string::npos ) {
                            act += ':' + msg.substr( dstart, dend - dstart );
                        }
                    }
                }
                if( act != "worn_sync" ) {
                    g_last_client_action_label = act;
                }
            }
        }
    }

    // Sync client facing direction so the remote NPC proxy flips correctly.
    if( jo.has_int( "client_facing" ) ) {
        remote->facing = jo.get_int( "client_facing" ) == 0
                         ? FacingDirection::LEFT : FacingDirection::RIGHT;
    }

    // Worn-item sync — client sends this once after joining (and after any
    // wear/take-off) so the remote NPC reflects the client's actual equipment.
    if( msg.find( "\"action\":\"worn_sync\"" ) != std::string::npos ) {
        mp_log( "[cdda-mp] worn_sync recv: " + msg.substr( 0, 120 ) );
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            // Sync gender so tileset uses the correct overlay prefix.
            if( jo.has_bool( "male" ) ) {
                remote->male = jo.get_bool( "male" );
            }
            if( jo.has_array( "worn" ) ) {
                remote->clear_worn();
                for( const JsonValue &wv : jo.get_array( "worn" ) ) {
                    JsonObject wo = wv.get_object();
                    wo.allow_omitted_members();
                    const itype_id tid( wo.get_string( "t", "" ) );
                    if( tid.is_valid() ) {
                        item worn_item( tid );
                        const std::string var = wo.get_string( "v", "" );
                        if( !var.empty() ) {
                            worn_item.set_itype_variant( var );
                        }
                        // wear_item(who, item, interactive, do_calc_encumbrance, do_sort, quiet)
                        auto result = remote->worn.wear_item( *remote, worn_item,
                                                             false, false, true, true );
                        if( !result ) {
                            mp_log( "[cdda-mp] worn_sync: wear_item FAILED for " + tid.str() );
                        }
                    }
                }
                std::vector<item *> applied_worn;
                remote->worn.inv_dump( applied_worn );
                std::string worn_list;
                for( const item *wi : applied_worn ) {
                    worn_list += wi->typeId().str() + ' ';
                }
                mp_log( "[cdda-mp] worn_sync applied: [" + worn_list + "]" );
                // Log overlay IDs the NPC would generate (confirm tileset coverage).
                std::string ov_log;
                for( const auto &ov : remote->get_overlay_ids() ) {
                    ov_log += ov.first + ' ';
                }
                mp_log( "[cdda-mp] worn_sync overlays: [" + ov_log + "]" );
            }
            // Apply the client's wielded weapon to the remote NPC.
            std::string wielded_str;
            jo.read( "wielded", wielded_str );
            if( !wielded_str.empty() ) {
                const itype_id wid( wielded_str );
                if( wid.is_valid() ) {
                    remote->set_wielded_item( item( wid ) );
                    mp_log( "[cdda-mp] worn_sync: set wielded=" + wielded_str );
                }
            }
            // Apply all appearance mutations (skin tone, eye color, hair style/color,
            // facial hair) from the client's "appearance" array to the remote NPC proxy.
            if( jo.has_array( "appearance" ) ) {
                for( const JsonValue &av : jo.get_array( "appearance" ) ) {
                    JsonObject ao = av.get_object();
                    ao.allow_omitted_members();
                    const std::string mtype = ao.get_string( "type", "" );
                    const std::string mid   = ao.get_string( "id",   "" );
                    if( mtype.empty() || mid.empty() ) {
                        continue;
                    }
                    const trait_id tid( mid );
                    if( !tid.is_valid() ) {
                        mp_log( "[cdda-mp] worn_sync: appearance id INVALID: " + mid );
                        continue;
                    }
                    // Clear any existing mutations of this type on the proxy.
                    for( const trait_id &old : get_mutations_in_type( mtype ) ) {
                        if( remote->has_trait( old ) ) {
                            remote->unset_mutation( old );
                        }
                    }
                    const std::string var_str = ao.get_string( "var", "" );
                    const mutation_variant *var = var_str.empty()
                                                  ? nullptr
                                                  : tid.obj().variant( var_str );
                    remote->set_mutation( tid, var );
                    mp_log( "[cdda-mp] worn_sync: appearance type=" + mtype
                            + " id=" + mid + " var=" + ( var ? var->id : "" ) );
                }
            }
        } catch( const JsonError &e ) {
            mp_log( std::string( "[cdda-mp] worn_sync parse error: " ) + e.what() );
        }
        return;
    }

    // Apply client's move_mode to the remote NPC proxy (run/crouch/prone overlay).
    if( msg.find( "\"move_mode\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            std::string mm_str;
            jo.read( "move_mode", mm_str );
            if( !mm_str.empty() ) {
                const move_mode_id mode_id( mm_str );
                if( mode_id.is_valid() ) {
                    remote->move_mode = mode_id;
                }
            }
        } catch( const JsonError & ) {}
    }

    // Mirror client's locally-computed stamina onto the proxy so the host sees accurate state.
    if( msg.find( "\"client_stamina\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_int( "client_stamina" ) ) {
                remote->set_stamina( jo.get_int( "client_stamina" ) );
            }
        } catch( const JsonError & ) {}
    }

    // Sync client light level so the host lighting pass can inject it at the proxy NPC.
    if( msg.find( "\"client_light\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_float( "client_light" ) || jo.has_int( "client_light" ) ) {
                g_mp_remote_player_luminance = static_cast<float>( jo.get_float( "client_light" ) );
            }
        } catch( const JsonError & ) {}
    }

    // Sync client bleeding effects to the remote NPC proxy so blood appears on
    // the host's map when the proxy moves.
    if( msg.find( "\"client_bleed\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_array( "client_bleed" ) ) {
                remote->remove_effect( effect_bleed );
                for( const JsonValue &bv : jo.get_array( "client_bleed" ) ) {
                    JsonObject bo = bv.get_object();
                    bo.allow_omitted_members();
                    const std::string bp_str = bo.get_string( "bp", "" );
                    const int intensity = bo.get_int( "intensity", 0 );
                    if( !bp_str.empty() && intensity > 0 ) {
                        const bodypart_id bp = bodypart_str_id( bp_str ).id();
                        remote->add_effect( effect_bleed, 2_turns, bp, intensity );
                    }
                }
            }
        } catch( const JsonError & ) {}
    }

    // Apply field changes (blood, etc.) placed on the client's map tiles.
    if( msg.find( "\"client_tile_changes\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_array( "client_tile_changes" ) ) {
                map &m = get_map();
                for( const JsonValue &entry : jo.get_array( "client_tile_changes" ) ) {
                    JsonObject to = entry.get_object();
                    to.allow_omitted_members();
                    const tripoint_abs_ms abs{
                        to.get_int( "x" ), to.get_int( "y" ), to.get_int( "z" )
                    };
                    if( !m.inbounds( abs ) ) {
                        continue;
                    }
                    const tripoint_bub_ms bub = m.get_bub( abs );
                    if( to.has_string( "ter" ) ) {
                        const ter_id tid( to.get_string( "ter" ) );
                        if( tid.id().is_valid() ) {
                            m.ter_set( bub, tid );
                            g_tile_baseline.erase( abs );
                        }
                    }
                    if( to.has_string( "furn" ) ) {
                        const furn_id fid( to.get_string( "furn" ) );
                        if( fid.id().is_valid() ) {
                            m.furn_set( bub, fid );
                            g_tile_baseline.erase( abs );
                        }
                    }
                    if( to.has_array( "items" ) ) {
                        mp_log( "[cdda-mp] server apply client items @ " +
                                std::to_string( abs.x() ) + "," +
                                std::to_string( abs.y() ) + "," +
                                std::to_string( abs.z() ) );
                        m.i_clear( bub );
                        for( const JsonValue &iv : to.get_array( "items" ) ) {
                            try {
                                item new_item;
                                JsonObject io = iv.get_object();
                                io.allow_omitted_members();
                                new_item.deserialize( io );
                                if( !new_item.typeId().is_empty() && new_item.typeId().is_valid() ) {
                                    m.add_item( bub, std::move( new_item ) );
                                }
                            } catch( const JsonError & ) {}
                        }
                        g_tile_baseline.erase( abs );
                    }
                    if( to.has_array( "fields" ) ) {
                        for( const JsonValue &fv : to.get_array( "fields" ) ) {
                            JsonObject fo = fv.get_object();
                            fo.allow_omitted_members();
                            const std::string type_str = fo.get_string( "t", "" );
                            if( type_str.empty() ) {
                                continue;
                            }
                            const field_type_id ftid( type_str );
                            if( ftid.is_valid() ) {
                                m.add_field( bub, ftid, fo.get_int( "i", 1 ) );
                                g_tile_baseline.erase( abs );
                            }
                        }
                    }
                    if( to.has_string( "trap" ) ) {
                        const std::string trap_str = to.get_string( "trap" );
                        if( trap_str.empty() || trap_str == "tr_null" ) {
                            m.trap_set( bub, tr_null );
                        } else {
                            const trap_str_id tsid( trap_str );
                            if( tsid.is_valid() ) {
                                m.trap_set( bub, tsid.id() );
                            }
                        }
                        g_tile_baseline.erase( abs );
                    }
                    if( to.has_string( "graffiti" ) ) {
                        const std::string gtext = to.get_string( "graffiti" );
                        if( gtext.empty() ) {
                            m.delete_graffiti( bub );
                        } else {
                            m.set_graffiti( bub, gtext );
                        }
                        g_tile_baseline.erase( abs );
                    }
                }
            }
        } catch( const JsonError & ) {}
    }

    // Apply client-reported combat damage (gun fire, throw, spell) to server monsters.
    if( msg.find( "\"client_monster_hits\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_array( "client_monster_hits" ) ) {
                map &m = get_map();
                bool any_killed = false;
                for( const JsonValue &hv : jo.get_array( "client_monster_hits" ) ) {
                    JsonObject ho = hv.get_object();
                    ho.allow_omitted_members();
                    const uint32_t nid = static_cast<uint32_t>( ho.get_int( "nid", 0 ) );
                    const int new_hp   = ho.get_int( "hp", -1 );
                    if( nid == 0 || new_hp < 0 ) {
                        continue;
                    }
                    monster *mon = nullptr;
                    for( const auto &ptr : get_creature_tracker().get_monsters_list() ) {
                        if( ptr && ptr->mp_net_id == nid ) {
                            mon = ptr.get();
                            break;
                        }
                    }
                    if( !mon || mon->is_dead() ) {
                        continue;
                    }
                    if( new_hp <= 0 ) {
                        mon->die( &m, nullptr );
                        any_killed = true;
                    } else {
                        mon->set_hp( new_hp );
                    }
                    mp_log( "[cdda-mp] client hit: nid=" + std::to_string( nid )
                           + " hp=" + std::to_string( new_hp ) );
                }
                if( any_killed ) {
                    g->cleanup_dead();
                }
            }
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] monster_hits parse error: " + std::string( e.what() ) );
        }
    }

    // Wait — drain one turn's worth of AP.
    const bool is_wait = msg.find( "\"action\":\"wait\"" ) != std::string::npos ||
                         msg.find( "\"action\": \"wait\"" ) != std::string::npos;
    if( is_wait ) {
        mp_log( "[cdda-mp] wait recv: ctrl_veh=" + std::to_string( remote->controlling_vehicle ) +
                " g_remote_moves=" + std::to_string( g_remote_moves ) );
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Pickup — drain AP and remove the taken items from the server's authoritative tile.
    if( msg.find( "\"action\":\"pickup\"" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            // Use NPC's server-side position — client coordinates may be out of sync.
            map &here = get_map();
            const tripoint_bub_ms bub_pos = remote->pos_bub();
            if( jo.has_array( "items" ) ) {
                for( const JsonValue &iv : jo.get_array( "items" ) ) {
                    JsonObject io = iv.get_object();
                    io.allow_omitted_members();
                    const itype_id tid( io.get_string( "t", "" ) );
                    if( !tid.is_valid() ) {
                        continue;
                    }
                    map_stack stack = here.i_at( bub_pos );
                    for( auto it = stack.begin(); it != stack.end(); ++it ) {
                        if( it->typeId() == tid ) {
                            here.i_rem( bub_pos, &*it );
                            mp_log( "[cdda-mp] pickup: removed " + tid.str()
                                    + " from " + std::to_string( bub_pos.x() ) + ","
                                    + std::to_string( bub_pos.y() ) );
                            break;
                        }
                    }
                }
            }
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] pickup parse error: " + std::string( e.what() ) );
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Drop — client dropped items locally; add them to the server's authoritative tile.
    // Always drop at the remote NPC's current server-side position to avoid coordinate
    // desync between the client's local bubble and the server's bubble.
    if( msg.find( "\"action\":\"drop\"" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            map &here = get_map();
            // Use the absolute coordinates sent by the client; fall back to NPC
            // proxy position only if the client didn't supply them.
            const tripoint_abs_ms remote_abs = remote->pos_abs();
            const tripoint_abs_ms abs_pos{
                jo.get_int( "x", remote_abs.x() ),
                jo.get_int( "y", remote_abs.y() ),
                jo.get_int( "z", remote_abs.z() )
            };
            const tripoint_bub_ms bub_pos = here.inbounds( abs_pos )
                                            ? here.get_bub( abs_pos )
                                            : remote->pos_bub();
            mp_log( "[cdda-mp] drop recv: abs=(" + std::to_string( abs_pos.x() ) + ","
                    + std::to_string( abs_pos.y() ) + "," + std::to_string( abs_pos.z() ) + ")"
                    + " bub=(" + std::to_string( bub_pos.x() ) + "," + std::to_string( bub_pos.y() ) + ")"
                    + " has_items=" + std::string( jo.has_array( "items" ) ? "yes" : "NO" ) );
            if( jo.has_array( "items" ) ) {
                for( const JsonValue &iv : jo.get_array( "items" ) ) {
                    try {
                        item dropped;
                        JsonObject io = iv.get_object();
                        io.allow_omitted_members();
                        dropped.deserialize( io );
                        if( !dropped.typeId().is_empty() && dropped.typeId().is_valid() ) {
                            mp_log( "[cdda-mp] drop: added " + dropped.typeId().str()
                                    + " at " + std::to_string( bub_pos.x() ) + ","
                                    + std::to_string( bub_pos.y() ) );
                            here.add_item( bub_pos, std::move( dropped ) );
                            g_tile_baseline.erase( abs_pos );
                        }
                    } catch( const JsonError &e ) {
                        mp_log( "[cdda-mp] drop: item deserialize error: " + std::string( e.what() ) );
                    }
                }
            }
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] drop parse error: " + std::string( e.what() ) );
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Door open/close.
    const bool is_open  = msg.find( "\"action\":\"open\""  ) != std::string::npos;
    const bool is_close = msg.find( "\"action\":\"close\"" ) != std::string::npos;
    if( is_open || is_close ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            const tripoint_abs_ms abs_target{
                jo.get_int( "x" ), jo.get_int( "y" ), jo.get_int( "z" )
            };
            if( m.inbounds( abs_target ) ) {
                const tripoint_bub_ms bub = m.get_bub( abs_target );
                if( is_open ) {
                    m.open_door( *remote, bub, true, false );
                } else {
                    // doors::close_door handles vehicle parts (next_part_to_close →
                    // veh->close()) as well as regular terrain/furniture doors.
                    // map::close_door alone skips vehicle doors entirely.
                    doors::close_door( m, *remote, bub );
                }
            }
        } catch( const JsonError &e ) {
            std::cout << "[cdda-mp] door parse error: " << e.what() << std::endl;
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Smash — parse absolute target and bash it using the client's smash ability.
    if( msg.find( "\"action\":\"smash\"" ) != std::string::npos ) {
        std::string smash_result_str = "failed";
        tripoint_abs_ms smash_target_abs{ 0, 0, 0 };
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            const tripoint_abs_ms abs_target{
                jo.get_int( "x" ), jo.get_int( "y" ), jo.get_int( "z" )
            };
            smash_target_abs = abs_target;
            if( m.inbounds( abs_target ) ) {
                const tripoint_bub_ms bub = m.get_bub( abs_target );
                // Capture target name BEFORE bash (it may be destroyed/replaced).
                std::string target_name;
                if( m.has_furn( bub ) ) {
                    target_name = m.furnname( bub );
                } else {
                    target_name = m.tername( bub );
                }
                auto bash_map = remote->smash_ability();
                if( jo.has_int( "bash" ) ) {
                    const int client_bash = jo.get_int( "bash" );
                    const damage_type_id bash_type( "bash" );
                    bash_map[bash_type] = client_bash;
                }
                // destroy=false so normal bash strength checks apply.
                const bash_params result = m.bash( bub, bash_map, false, false );
                if( result.success ) {
                    smash_result_str = "destroyed";
                } else if( result.did_bash ) {
                    smash_result_str = result.can_bash ? "hit" : "impossible";
                }
                mp_log( "[cdda-mp] smash @ " +
                        std::to_string( abs_target.x() ) + "," +
                        std::to_string( abs_target.y() ) +
                        " result=" + smash_result_str );
                // Generate a message so flush_action_msgs forwards it to the client.
                if( !target_name.empty() ) {
                    if( smash_result_str == "destroyed" ) {
                        add_msg( m_good, _( "%s smashes the %s to pieces!" ),
                                 remote->get_name(), target_name );
                    } else if( smash_result_str == "hit" ) {
                        add_msg( _( "%s strikes the %s." ),
                                 remote->get_name(), target_name );
                    } else if( smash_result_str == "impossible" ) {
                        add_msg( m_info, _( "%s can't damage the %s." ),
                                 remote->get_name(), target_name );
                    }
                }
            }
        } catch( const JsonError &e ) {
            std::cout << "[cdda-mp] smash parse error: " << e.what() << std::endl;
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        flush_action_msgs( pre_action_msg, remote->name );
        server *srv = get_active_server();
        if( srv ) {
            std::string state = serialize_remote_player_state();
            // Inject smash result + target position so the client can animate the hit.
            state = state.substr( 0, state.size() - 1 )
                    + ",\"smash_result\":\"" + smash_result_str + "\""
                    + ",\"smash_x\":" + std::to_string( smash_target_abs.x() )
                    + ",\"smash_y\":" + std::to_string( smash_target_abs.y() )
                    + ",\"smash_z\":" + std::to_string( smash_target_abs.z() ) + "}";
            srv->post_broadcast( state + "\n" );
        }
        return;
    }

    // Position sync — client went up/down stairs; teleport proxy NPC to match.
    if( msg.find( "\"action\":\"position_sync\"" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            const tripoint_abs_ms abs_pos{
                jo.get_int( "x" ), jo.get_int( "y" ), jo.get_int( "z" )
            };
            if( m.inbounds( abs_pos ) ) {
                remote->setpos( m, m.get_bub( abs_pos ) );
                std::cout << "[cdda-mp] position_sync: proxy NPC moved to ("
                          << abs_pos.x() << "," << abs_pos.y() << "," << abs_pos.z()
                          << ")" << std::endl;
            }
        } catch( const JsonError &e ) {
            std::cout << "[cdda-mp] position_sync parse error: " << e.what() << std::endl;
        }
        // Count as the client's action for this turn so wait_for_client_action() unblocks.
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        g_remote_moves = 0;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Eat / use item — client is authoritative over its own nutrition; server just
    // drains one turn of AP and re-broadcasts state so the client's HUD stays current.
    if( msg.find( "\"action\":\"eat\"" ) != std::string::npos ) {
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Control vehicle — toggle the proxy NPC's vehicle control state.
    // Mirrors game::control_vehicle() for the client's proxy.
    {
        const bool cv_match = msg.find( "\"action\":\"control_vehicle\"" ) != std::string::npos;
        mp_log( "[cdda-mp] DEBUG ctrl_veh check: match=" + std::to_string( cv_match )
                + " msg[0..60]=" + msg.substr( 0, 60 ) );
    }
    if( msg.find( "\"action\":\"control_vehicle\"" ) != std::string::npos ) {
        mp_log( "[cdda-mp] control_vehicle: HANDLER ENTERED controlling=" +
                std::to_string( remote->controlling_vehicle ) );
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        mp_log( "[cdda-mp] control_vehicle: proxy bub=(" + std::to_string( bub.x() ) + "," +
                std::to_string( bub.y() ) + "," + std::to_string( bub.z() ) + ")" );
        const optional_vpart_position vp_check = here.veh_at( bub );
        mp_log( "[cdda-mp] control_vehicle: veh_at=" + std::to_string( static_cast<bool>( vp_check ) ) );
        if( remote->controlling_vehicle ) {
            // Already controlling — give up control and unboard.
            remote->controlling_vehicle = false;
            here.unboard_vehicle( bub );
            remote->in_vehicle = false;
            mp_log( "[cdda-mp] control_vehicle: proxy released controls" );
            // Host log: NPC-form. Client: direct push (correct grammar, first-person).
            add_msg( _( "%s lets go of the controls." ), remote->name );
            g_action_msgs_pending.push_back( _( "You let go of the controls." ) );
        } else if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            const int ctrl_idx = veh.avail_part_with_feature( vp->mount_pos(), "CONTROLS" );
            mp_log( "[cdda-mp] control_vehicle: ctrl_idx=" + std::to_string( ctrl_idx ) +
                    " engine_on=" + std::to_string( veh.engine_on ) );
            if( ctrl_idx >= 0 ) {
                if( !remote->in_vehicle ) {
                    here.board_vehicle( bub, remote );  // register as passenger so vehicle moves NPC
                }
                remote->in_vehicle = true;
                const bool engine_was_off = !veh.engine_on;
                if( engine_was_off ) {
                    // start_engines() with an NPC driver assigns a cranking activity whose
                    // finish() uses get_player_character() to re-find the vehicle — it
                    // never finds it for an NPC proxy and the engine stays off.
                    // Bypass the activity: directly enable and start each engine, mirroring
                    // what start_engines_activity_actor::finish() does internally.
                    for( const int p : veh.engines ) {
                        vehicle_part &vpart = veh.part( p );
                        if( !vpart.is_broken() ) {
                            vpart.enabled = true;
                        }
                    }
                    int started = 0;
                    for( const int p : veh.engines ) {
                        vehicle_part &vpart = veh.part( p );
                        if( veh.is_engine_on( vpart ) && veh.start_engine( here, vpart ) ) {
                            started++;
                        }
                    }
                    veh.engine_on = started > 0;
                    mp_log( "[cdda-mp] control_vehicle: direct engine start, started=" +
                            std::to_string( started ) + " engine_on=" + std::to_string( veh.engine_on ) );
                }
                if( veh.engine_on ) {
                    remote->controlling_vehicle = true;
                    mp_log( "[cdda-mp] control_vehicle: proxy took/started+took control of " + veh.name );
                    // Host log: NPC-form. Client: direct push (correct grammar, first-person).
                    add_msg( _( "%s takes control of the %s." ), remote->name, veh.name );
                    g_action_msgs_pending.push_back(
                        string_format( _( "You take control of the %s." ), veh.name ) );
                    if( engine_was_off ) {
                        add_msg( _( "The %s's engine starts up." ), veh.name );
                        g_action_msgs_pending.push_back(
                            string_format( _( "The %s's engine starts up." ), veh.name ) );
                    }
                } else {
                    mp_log( "[cdda-mp] control_vehicle: engine failed to start" );
                    add_msg( m_bad, _( "%s can't start the %s's engine." ), remote->name, veh.name );
                    g_action_msgs_pending.push_back(
                        string_format( _( "You can't start the %s's engine." ), veh.name ) );
                }
            } else {
                mp_log( "[cdda-mp] control_vehicle: no controls at proxy position" );
                add_msg( m_info, _( "%s can't drive from here — no controls." ), remote->name );
                g_action_msgs_pending.push_back( _( "You can't drive from here — no controls." ) );
            }
        } else {
            mp_log( "[cdda-mp] control_vehicle: no vehicle at proxy position" );
            add_msg( m_info, _( "No vehicle here for %s to control." ), remote->name );
            g_action_msgs_pending.push_back( _( "No vehicle here to control." ) );
        }
        // Don't use flush_action_msgs here — messages are pushed directly above
        // to avoid grammar issues from NPC-name→"You" substitution ("takes"→"take").
        g_last_forwarded_msg_count = Messages::size();
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Pldrive — client is at vehicle controls, translate directional input to
    // vehicle::pldrive() exactly as handle_action.cpp does in single-player.
    if( msg.find( "\"action\":\"pldrive\"" ) != std::string::npos ) {
        int dx = 0;
        int dy = 0;
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            dx = jo.get_int( "dx", 0 );
            dy = jo.get_int( "dy", 0 );
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] pldrive parse error: " + std::string( e.what() ) );
        }
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            const int face_before = static_cast<int>( units::to_degrees( veh.face.dir() ) );
            const int str = remote->get_str();
            const int dex = remote->get_dex();
            const int drv = remote->get_skill_level( skill_id( "driving" ) );
            mp_log( "[cdda-mp] pldrive: dx=" + std::to_string( dx ) +
                    " dy=" + std::to_string( dy ) +
                    " face=" + std::to_string( face_before ) +
                    " str=" + std::to_string( str ) +
                    " dex=" + std::to_string( dex ) +
                    " drv=" + std::to_string( drv ) +
                    " moves=" + std::to_string( g_remote_moves ) );
            // pldrive() internally caps moves to get_speed() then deducts the turn cost.
            // Preserve any budget above one speed unit, then use pldrive's remainder.
            const int excess = std::max( 0, g_remote_moves - remote->get_speed() );
            veh.pldrive( here, *remote, dx, dy, 0 );
            mp_log( "[cdda-mp] pldrive result: face=" +
                    std::to_string( static_cast<int>( units::to_degrees( veh.face.dir() ) ) ) +
                    " moves_after=" + std::to_string( remote->get_moves() ) );
            g_remote_moves = excess + remote->get_moves();
        } else {
            // No longer at a vehicle — clear control flag so the client gets corrected.
            remote->controlling_vehicle = false;
            g_remote_moves -= remote->get_speed();
        }
        g_client_acted_this_turn = true;
        flush_action_msgs( pre_action_msg, remote->name );
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Cruise speed adjustment — free action (mirrors SP: cruise_thrust costs no AP).
    if( msg.find( "\"action\":\"cruise\"" ) != std::string::npos ) {
        int dy = 0;
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            dy = jo.get_int( "dy", 0 );
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] cruise parse error: " + std::string( e.what() ) );
        }
        map &here = get_map();
        if( const optional_vpart_position vp = here.veh_at( remote->pos_bub() ) ) {
            vehicle &veh = vp->vehicle();
            veh.cruise_thrust( here, -dy * 400 );
            mp_log( "[cdda-mp] cruise: dy=" + std::to_string( dy )
                    + " cruise_vel=" + std::to_string( veh.cruise_velocity ) );
        }
        // Free action: do NOT deduct g_remote_moves.  Broadcast updated state.
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Handbrake — client pressed smash key while in vehicle control mode.
    if( msg.find( "\"action\":\"handbrake\"" ) != std::string::npos ) {
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            mp_log( "[cdda-mp] handbrake: veh=" + veh.name
                    + " vel=" + std::to_string( veh.velocity ) );
            add_msg( _( "%s pulls a handbrake." ), remote->name );
            g_action_msgs_pending.push_back( _( "You pull a handbrake." ) );
            veh.cruise_velocity = 0;
            if( veh.last_turn != 0_degrees &&
                rng( 15, 60 ) * 100 < std::abs( veh.velocity ) ) {
                veh.skidding = true;
                add_msg( m_warning, _( "%s loses control of %s." ), remote->name, veh.name );
                g_action_msgs_pending.push_back( string_format( _( "You lose control of %s." ),
                                                 veh.name ) );
                veh.turn( veh.last_turn > 0_degrees ? 60_degrees : -60_degrees );
            } else {
                int braking_power = std::abs( veh.velocity ) / 2 + 10 * 100;
                if( std::abs( veh.velocity ) < braking_power ) {
                    veh.stop( here );
                } else {
                    int sgn = veh.velocity > 0 ? 1 : -1;
                    veh.velocity = sgn * ( std::abs( veh.velocity ) - braking_power );
                }
            }
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        g_last_forwarded_msg_count = Messages::size();
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    // Stop engine — client selected "Stop engine" from driving menu.
    if( msg.find( "\"action\":\"stop_engine\"" ) != std::string::npos ) {
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            veh.engine_on = false;
            for( const int p : veh.engines ) {
                veh.part( p ).enabled = false;
            }
            veh.cruise_velocity = 0;
            mp_log( "[cdda-mp] stop_engine: engines stopped for " + veh.name );
            // Host log: NPC-form. Client: direct push (correct grammar, first-person).
            add_msg( _( "%s turns off the engine and lets go of the controls." ), remote->name );
            g_action_msgs_pending.push_back(
                string_format( _( "You turn the engine off and let go of the controls." ) ) );
        }
        remote->controlling_vehicle = false;
        here.unboard_vehicle( bub );
        remote->in_vehicle = false;
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        g_last_forwarded_msg_count = Messages::size();
        server *srv = get_active_server();
        if( srv ) { srv->post_broadcast( serialize_remote_player_state() + "\n" ); }
        return;
    }

    // Toggle engine while not driving (at controls but not in control mode).
    if( msg.find( "\"action\":\"toggle_engine\"" ) != std::string::npos ) {
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            if( veh.engine_on ) {
                veh.stop_engines( here );
                add_msg( _( "%s turns off the engine." ), remote->name );
                g_action_msgs_pending.push_back( _( "You turn the engine off." ) );
            } else {
                veh.start_engines( here, remote );
                add_msg( _( "%s starts the engine." ), remote->name );
                g_action_msgs_pending.push_back( _( "You start the engine." ) );
            }
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        g_last_forwarded_msg_count = Messages::size();
        server *srv = get_active_server();
        if( srv ) { srv->post_broadcast( serialize_remote_player_state() + "\n" ); }
        return;
    }

    // Honk horn.
    if( msg.find( "\"action\":\"honk\"" ) != std::string::npos ) {
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            veh.honk_horn( here );
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        flush_action_msgs( pre_action_msg, remote->name );
        server *srv = get_active_server();
        if( srv ) { srv->post_broadcast( serialize_remote_player_state() + "\n" ); }
        return;
    }

    tripoint_bub_ms cur = remote->pos_bub();
    tripoint_bub_ms next = cur;

    // Sync NPC proxy move_mode to what the client committed before dispatching.
    // If stamina later runs out during the move, the auto-walk transition below
    // will override this and the state packet will correct the client.
    {
        const size_t mm = msg.find( "\"move_mode\":\"" );
        if( mm != std::string::npos ) {
            const size_t start = mm + 13;
            const size_t end = msg.find( '"', start );
            if( end != std::string::npos ) {
                try {
                    remote->move_mode = move_mode_id( msg.substr( start, end - start ) );
                } catch( ... ) {}
            }
        }
    }

    // Parse direction — handle both "dir":"n" and "dir": "n" spacing
    const auto dir_match = [&msg]( const std::string & d ) {
        return msg.find( "\"dir\":\"" + d + "\"" ) != std::string::npos ||
               msg.find( "\"dir\": \"" + d + "\"" ) != std::string::npos;
    };
    tripoint offset;
    if( dir_match( "n" ) ) {
        offset = tripoint( 0, -1, 0 );
    } else if( dir_match( "s" ) ) {
        offset = tripoint( 0, 1, 0 );
    } else if( dir_match( "e" ) ) {
        offset = tripoint( 1, 0, 0 );
    } else if( dir_match( "w" ) ) {
        offset = tripoint( -1, 0, 0 );
    } else if( dir_match( "ne" ) ) {
        offset = tripoint( 1, -1, 0 );
    } else if( dir_match( "nw" ) ) {
        offset = tripoint( -1, -1, 0 );
    } else if( dir_match( "se" ) ) {
        offset = tripoint( 1, 1, 0 );
    } else if( dir_match( "sw" ) ) {
        offset = tripoint( -1, 1, 0 );
    }
    next += offset;

    // Track whether this action consumed a turn.  Wall bumps and other no-ops
    // must not lock the client — they get a "free":true response instead.
    bool acted = false;

    // Move or attack, matching single-player bump-to-attack behaviour.
    // Check for a creature first — melee_attack applies regardless of tile passability.
    if( next != cur ) {
        const tripoint_abs_ms next_abs = m.get_abs( next );
        // Block movement onto the host avatar's tile.
        const tripoint_abs_ms host_abs = m.get_abs( get_avatar().pos_bub() );
        if( next_abs == host_abs ) {
            // TODO: trigger an interaction menu on the host side here.
            // For now, refuse the move and send a corrected state so the client snaps back.
            server *srv = get_active_server();
            if( srv ) {
                srv->post_broadcast( serialize_remote_player_state() + "\n" );
            }
            return;
        }
        const shared_ptr_fast<monster> target = get_creature_tracker().find( next_abs );
        if( target ) {
            // melee_attack() charges moves on the NPC internally; capture the result.
            remote->melee_attack( *target, true );
            g_remote_moves = remote->get_moves();
            acted = true;
        } else if( !m.impassable( next ) ) {
            remote->setpos( m, next );
            // Trigger traps on the destination tile, mirroring game.cpp:8351.
            m.creature_on_trap( *remote );
            // Use combined_movecost (same as game.cpp:7733) so the AP cost includes
            // all terrain/encumbrance factors, not just the raw tile cost.
            const bool diag = ( std::abs( offset.x ) + std::abs( offset.y ) ) == 2;
            const int mcost = m.combined_movecost( cur, next );
            const int prev_moves = g_remote_moves;
            const int ap_cost = remote->run_cost( mcost, diag );
            g_remote_moves -= ap_cost;
            // burn_move_stamina with the actual AP consumed, mirroring game.cpp:7776.
            remote->burn_move_stamina( prev_moves - g_remote_moves );
            // Auto-transition to walk when stamina runs out (mirrors game.cpp:8970).
            static const move_mode_id walk_id( "walk" );
            if( !remote->can_run() ) {
                remote->move_mode = walk_id;
            }
            acted = true;
        } else {
            // Bump-to-open: try to open a door on the target tile (follows CDDA rules —
            // respects locks, handles etc).  If it's not openable, it's a wall bump.
            if( m.open_door( *remote, next, true, false ) ) {
                g_remote_moves -= remote->get_speed();
                acted = true;
            }
            // else: solid wall — acted stays false, no AP charged
        }
    }

    // Capture all messages generated during this action (hits, misses, damage,
    // kills) for forwarding to the client regardless of NPC name filter.
    flush_action_msgs( pre_action_msg, remote->name );

    if( acted ) {
        g_client_acted_this_turn = true;
        g_remote_moves = 0;  // lock client; grant_client_turn() restores on next turn
    }

    // Broadcast updated state.  Wall bumps and other no-ops include "free":true
    // so the client knows to restore its moves without waiting for the next turn grant.
    server *srv = get_active_server();
    if( srv ) {
        std::string state = serialize_remote_player_state();
        if( !acted ) {
            state = state.substr( 0, state.size() - 1 ) + ",\"free\":true}";
        }
        srv->post_broadcast( state + "\n" );
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void grant_client_turn()
{
    if( !remote_player_connected ) {
        return;
    }
    npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
    if( !remote ) {
        return;
    }
    g_client_acted_this_turn = false;
    g_remote_moves = remote->get_speed();
    mp_log( "[cdda-mp] grant_client_turn: remote_moves=" + std::to_string( g_remote_moves ) );
    // Proxy skips npcmove so never auto-regenerates stamina. Replicate the
    // update_body() path that the real avatar gets each game turn.
    remote->update_stamina( 1 );
    server *srv = get_active_server();
    if( srv ) {
        srv->post_broadcast( serialize_remote_player_state() + "\n" );
    }
}

void wait_for_client_action()
{
    if( !remote_player_connected ) {
        return;
    }

    // During host sleep, don't block — just drain the event queue and return.
    // Sleep is fast-forwarded (8 in-game hours in <1 s wall-clock).  Blocking
    // up to 10 s per tick × ~28800 ticks would freeze the game for hours.
    // Short wait activities (ACT_WAIT, ACT_WAIT_STAMINA, etc.) stay in lockstep:
    // the client's 500 ms auto-wait fires once per activity tick, so the client
    // gets one free turn per tick (~30 real seconds for a 1-minute in-game wait).
    {
        static const efftype_id eff_sleep( "sleep" );
        if( get_avatar().has_effect( eff_sleep ) ) {
            process_mp_events();
            mp_log( "[cdda-mp] host sleeping — skipping client wait (fast-forward)" );
            return;
        }
    }

    g_host_waiting_for_client = true;
    using namespace std::chrono_literals;
    const auto t_start = std::chrono::steady_clock::now();
    auto deadline = t_start + 10s;
    while( !g_client_acted_this_turn && remote_player_connected ) {
        if( std::chrono::steady_clock::now() >= deadline ) {
            std::cout << "[cdda-mp] lockstep: timed out waiting for client action" << std::endl;
            break;
        }
        process_mp_events();
        if( g_client_acted_this_turn ) {
            break;
        }
        ensure_mp_hud();
        // Pump SDL events and dispatch any pending input so the host can use
        // UI keys (@, ?, map, etc.) while waiting for the client.  The guard in
        // handle_action() allows only pure-UI actions when moves<=0, so world
        // state cannot be mutated from here.
        inp_mngr.pump_events();
        g->mp_poll_input();
        std::this_thread::sleep_for( 16ms );
    }
    g_host_waiting_for_client = false;
    g_wait_elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start ).count() );

    // Keep the remote NPC proxy's in_vehicle flag in sync with its map position
    // before monmove() / process_vehicles() runs.  vehicle_move.cpp line ~892 skips
    // collision for any character with in_vehicle==true, so without this the vehicle
    // rams into its own passenger each time it moves.
    if( remote_player_connected ) {
        npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
        if( remote ) {
            const optional_vpart_position ovp = get_map().veh_at( remote->pos_bub() );
            remote->in_vehicle = static_cast<bool>( ovp );
            if( !ovp ) {
                remote->controlling_vehicle = false;
            }
        }
    }
}

void set_last_monmove_ms( int ms )
{
    g_last_monmove_ms = ms;
}

bool is_remote_player( character_id id )
{
    return remote_player_connected && id == remote_player_npc_id;
}

bool is_client_host_at( const tripoint_abs_ms &abs )
{
    if( !client_host_npc_spawned || !client_host_npc_id.is_valid() ) {
        return false;
    }
    npc *hnpc = g->critter_by_id<npc>( client_host_npc_id );
    return hnpc && get_map().get_abs( hnpc->pos_bub() ) == abs;
}

void notify_client_host_died()
{
    server *srv = get_active_server();
    if( !srv ) {
        return;
    }
    srv->post_broadcast( "{\"type\":\"host_died\"}\n" );
    // Brief pause so the packet flushes before the death screen takes focus.
    std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
}

// Check separation between two absolute positions and update g_separation_tier.
// Uses Chebyshev distance (same as rl_dist in 2D).  Tier thresholds:
//   0 → 1 at ≥50 tiles,  1 → 2 at ≥57 tiles
//   2 → 1 at  <50 tiles, 1 → 0 at  <44 tiles  (hysteresis prevents flicker)
static void check_separation_warning( const tripoint_abs_ms &a, const tripoint_abs_ms &b )
{
    const int dist = std::max( std::abs( a.x() - b.x() ), std::abs( a.y() - b.y() ) );
    const int prev = g_separation_tier;
    if( dist >= 57 ) {
        g_separation_tier = 2;
    } else if( dist >= 50 ) {
        g_separation_tier = std::max( g_separation_tier, 1 );
    } else if( dist < 44 ) {
        g_separation_tier = 0;
    } else if( dist < 50 ) {
        g_separation_tier = std::min( g_separation_tier, 1 );
    }
    if( g_separation_tier != prev ) {
        if( g_separation_tier == 0 ) {
            add_msg( m_good, "You and your partner are close enough again." );
        } else if( g_separation_tier == 1 ) {
            add_msg( m_warning, "Your partner is getting far away (%d tiles). Max safe range is ~60.", dist );
        } else {
            add_msg( m_bad, "Your partner is near the edge of the simulated zone (%d tiles)! Move closer.", dist );
        }
    }
}

void process_mp_events()
{
    // On the very first tick, purge any MP NPCs that leaked into the world save
    // from a previous session (server and client share the same world directory).
    mp_cleanup_stale_npcs();

    static int s_call_id = 0;
    const int call_id = ++s_call_id;
    mp_event event;
    while( get_mp_queue().pop( event ) ) {
        if( event.evt_type == mp_event::type::action ) {
            mp_log( "[cdda-mp] process_mp_events #" + std::to_string( call_id ) +
                    " popped: " + event.data.substr( 0, 60 ) );
        }
        switch( event.evt_type ) {
            case mp_event::type::connect:
                spawn_remote_player( event.session_id );
                break;
            case mp_event::type::disconnect:
                remove_remote_player();
                break;
            case mp_event::type::action:
                handle_remote_action( event.session_id, event.data );
                break;
        }
    }

    // Broadcast host position every turn so the client sees the host move in real time.
    if( remote_player_connected ) {
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        // Warn if the two players are drifting near the edge of the shared bubble.
        npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
        if( remote ) {
            check_separation_warning( get_avatar().pos_abs(), remote->pos_abs() );
        }
    }
}

static void apply_monster_sync( JsonObject &jo );
static void apply_tile_changes( JsonObject &jo );
static void apply_vehicle_sync( JsonObject &jo );

// Move the client avatar to an absolute position, loading the map chunk if needed.
static void client_teleport_avatar( const tripoint_abs_ms &abs_pos )
{
    avatar &u = get_avatar();
    map &m = get_map();

    std::cout << "[cdda-mp] teleport: abs=" << abs_pos.x() << "," << abs_pos.y()
              << " inbounds=" << m.inbounds( abs_pos )
              << " initial_done=" << g_initial_teleport_done << std::flush;

    if( !m.inbounds( abs_pos ) ) {
        if( !g_initial_teleport_done ) {
            std::cout << " → place_player_overmap..." << std::flush;
            g->place_player_overmap( project_to<coords::omt>( abs_pos ), true );
            g_initial_teleport_done = true;
            std::cout << " done" << std::flush;
        } else {
            std::cout << " → out of bubble, skip" << std::endl;
            return;
        }
    } else {
        g_initial_teleport_done = true;
    }

    const tripoint_bub_ms new_pos = m.get_bub( abs_pos );
    if( new_pos != u.pos_bub() ) {
        std::cout << " → setpos+update_map..." << std::flush;
        u.setpos( m, new_pos );
        g->update_map( u );
        std::cout << " done" << std::flush;
    } else {
        std::cout << " → already at target" << std::flush;
    }
    std::cout << std::endl;
}

static void remove_client_host_npc()
{
    if( !client_host_npc_spawned ) {
        return;
    }
    // Try to kill through the active tracker first; fall back to overmap removal.
    npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
    if( host_npc ) {
        host_npc->set_moves( 0 );
        host_npc->die( &get_map(), nullptr );
        g->cleanup_dead();
    } else if( client_host_npc_id.is_valid() ) {
        overmap_buffer.remove_npc( client_host_npc_id );
    }
    client_host_npc_spawned = false;
    client_host_npc_id = character_id();
    g_client_host_worn_sig.clear();
    mp_save_npc_ids();
}

static void update_client_host_npc( const tripoint_abs_ms &abs_pos, const std::string &name )
{
    map &m = get_map();

    if( !client_host_npc_spawned ) {
        // Purge any stale host-proxy NPCs with the same name from old sessions.
        purge_npcs_by_name( name.empty() ? "host" : name );

        shared_ptr_fast<npc> host_npc = make_shared_fast<npc>();
        host_npc->normalize();
        host_npc->name = name.empty() ? "host" : name;
        host_npc->spawn_at_precise( abs_pos );
        overmap_buffer.insert_npc( host_npc );
        g->load_npcs();
        client_host_npc_id = host_npc->getID();
        client_host_npc_spawned = true;
        mp_save_npc_ids();  // persist ID so next session can clean it up

        // Apply ally status AFTER load_npcs so the NPC is fully wired into the
        // game state (faction manager, critter tracker, etc.).
        npc *hn = g->critter_by_id<npc>( client_host_npc_id );
        if( hn ) {
            hn->set_attitude( NPCATT_FOLLOW );
            hn->set_fac( faction_id( "your_followers" ) );
            hn->chatbin.first_topic = "TALK_FRIEND";
            hn->op_of_u.trust = 10;
            hn->op_of_u.value = 10;
            g->add_npc_follower( hn->getID() );
        }
        std::cout << "[cdda-mp] Spawned host NPC '" << host_npc->name << "' at abs "
                  << abs_pos.x() << "," << abs_pos.y() << std::endl;
        return;
    }

    npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
    if( !host_npc ) {
        // NPC left the reality bubble — still in overmap buffer, just not loaded.
        // Don't reset spawned state or we'll create a duplicate on the next tick.
        // If they're far enough to load, request a map reload in their direction.
        if( m.inbounds( abs_pos ) ) {
            // Back in bounds — the game will load_npcs() on the next do_turn pass.
            g->load_npcs();
        }
        return;
    }
    if( !name.empty() && host_npc->name != name ) {
        host_npc->name = name;
    }
    // Re-apply ally status every position update — cheap and idempotent, ensures
    // settings survive save/load cycles where the NPC may reload with neutral defaults.
    if( !host_npc->is_player_ally() ) {
        host_npc->set_attitude( NPCATT_FOLLOW );
        host_npc->set_fac( faction_id( "your_followers" ) );
        host_npc->chatbin.first_topic = "TALK_FRIEND";
        host_npc->op_of_u.trust = 10;
        host_npc->op_of_u.value = 10;
        g->add_npc_follower( host_npc->getID() );
    }
    if( m.inbounds( abs_pos ) ) {
        const tripoint_bub_ms bub = m.get_bub( abs_pos );
        if( bub != host_npc->pos_bub() ) {
            host_npc->setpos( m, bub );
        }
    }
}

static bool apply_one_state_message( const std::string &msg )
{
    // Log a preview of every received packet so we can confirm moves=90 packets arrive.
    {
        const size_t preview_len = std::min( msg.size(), static_cast<size_t>( 120 ) );
        mp_log( "[cdda-mp] recv-packet: " + msg.substr( 0, preview_len ) );
    }
    // Server asks the client to re-send worn/hair after a respawn.
    if( msg.find( "\"type\":\"resync_request\"" ) != std::string::npos ) {
        client_resync_worn();
        return true;
    }

    // Host died gracefully — show once and stop processing further packets.
    if( msg.find( "\"type\":\"host_died\"" ) != std::string::npos ) {
        if( !g_server_died ) {
            g_server_died = true;
            remove_client_host_npc();
            add_msg( m_bad, "Your partner has died.  Waiting for them to respawn..." );
        }
        return true;
    }

    const bool is_state = msg.find( "\"type\":\"state\"" ) != std::string::npos ||
                          msg.find( "\"type\": \"state\"" ) != std::string::npos;
    if( !is_state ) {
        std::cout << "[cdda-mp] " << msg << std::endl;
        return false;
    }
    if( msg.find( "\"connected\":false" ) != std::string::npos ) {
        if( !g_server_died ) {
            g_server_died = true;
            remove_client_host_npc();
            add_msg( m_bad, "Lost connection to server." );
        }
        return true;
    }

    try {
        std::cout << "[cdda-mp] parsing state (" << msg.size() << " bytes)..." << std::flush;
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        std::cout << " ok" << std::endl;

        // Sync host's calendar turn so the client sees the correct time, lighting, and weather.
        if( jo.has_int( "calendar_turn" ) ) {
            calendar::turn = time_point( jo.get_int( "calendar_turn" ) );
        }

        if( jo.has_object( "pos" ) ) {
            std::cout << "[cdda-mp] teleporting avatar..." << std::flush;
            JsonObject pos = jo.get_object( "pos" );
            pos.allow_omitted_members();
            g_mp_remote_pos = tripoint_abs_ms{
                pos.get_int( "x" ), pos.get_int( "y" ), pos.get_int( "z" )
            };
            client_teleport_avatar( g_mp_remote_pos );
            std::cout << " ok" << std::endl;
        }

        std::string host_name;
        jo.read( "host_name", host_name );

        if( jo.has_object( "host_pos" ) ) {
            std::cout << "[cdda-mp] updating host NPC..." << std::flush;
            JsonObject hpos = jo.get_object( "host_pos" );
            hpos.allow_omitted_members();
            const tripoint_abs_ms host_pos{
                hpos.get_int( "x" ), hpos.get_int( "y" ), hpos.get_int( "z" )
            };
            update_client_host_npc( host_pos, host_name );
            std::cout << " ok" << std::endl;
        }

        if( jo.has_float( "host_light" ) || jo.has_int( "host_light" ) ) {
            g_mp_host_luminance = static_cast<float>( jo.get_float( "host_light" ) );
        }

        // Dress the host NPC with the items the host player is wearing and apply
        // all appearance mutations. Signature-gated to avoid redoing every tick.
        if( jo.has_array( "host_worn" ) ) {
            // Fingerprint: worn list + appearance array raw string + wielded.
            std::string sig;
            for( const JsonValue &wv : jo.get_array( "host_worn" ) ) {
                JsonObject wo = wv.get_object();
                wo.allow_omitted_members();
                sig += wo.get_string( "t", "" ) + ',';
            }
            std::string incoming_wielded;
            jo.read( "host_wielded", incoming_wielded );
            // Include the full appearance JSON in sig so any sub-field change triggers redress.
            if( jo.has_array( "host_appearance" ) ) {
                for( const JsonValue &av : jo.get_array( "host_appearance" ) ) {
                    JsonObject ao = av.get_object();
                    ao.allow_omitted_members();
                    sig += ao.get_string( "type", "" ) + ':' + ao.get_string( "id", "" )
                        + '/' + ao.get_string( "var", "" ) + '|';
                }
            }
            sig += '|' + incoming_wielded;

            if( sig != g_client_host_worn_sig && client_host_npc_spawned ) {
                std::cout << "[cdda-mp] dressing host NPC..." << std::flush;
                g_client_host_worn_sig = sig;
                npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
                if( host_npc ) {
                    if( jo.has_bool( "host_male" ) ) {
                        host_npc->male = jo.get_bool( "host_male" );
                    }
                    host_npc->clear_worn();
                    std::string applied_log;
                    for( const JsonValue &wv : jo.get_array( "host_worn" ) ) {
                        JsonObject wo = wv.get_object();
                        wo.allow_omitted_members();
                        const itype_id tid( wo.get_string( "t", "" ) );
                        if( tid.is_valid() ) {
                            item worn_item( tid );
                            const std::string var = wo.get_string( "v", "" );
                            if( !var.empty() ) {
                                worn_item.set_itype_variant( var );
                                applied_log += tid.str() + '[' + var + "] ";
                            } else {
                                applied_log += tid.str() + ' ';
                            }
                            host_npc->worn.wear_item( *host_npc, worn_item,
                                                      false, false, true, true );
                        }
                    }
                    mp_log( "[cdda-mp] host_worn applied: [" + applied_log + "]" );
                    // Apply all appearance mutations from the host_appearance array.
                    if( jo.has_array( "host_appearance" ) ) {
                        for( const JsonValue &av : jo.get_array( "host_appearance" ) ) {
                            JsonObject ao = av.get_object();
                            ao.allow_omitted_members();
                            const std::string mtype = ao.get_string( "type", "" );
                            const std::string mid   = ao.get_string( "id",   "" );
                            if( mtype.empty() || mid.empty() ) { continue; }
                            const trait_id tid( mid );
                            if( !tid.is_valid() ) { continue; }
                            for( const trait_id &old : get_mutations_in_type( mtype ) ) {
                                if( host_npc->has_trait( old ) ) {
                                    host_npc->unset_mutation( old );
                                }
                            }
                            const std::string var_str = ao.get_string( "var", "" );
                            const mutation_variant *var = var_str.empty()
                                                          ? nullptr
                                                          : tid.obj().variant( var_str );
                            host_npc->set_mutation( tid, var );
                            mp_log( "[cdda-mp] host_appearance applied: type=" + mtype
                                    + " id=" + mid );
                        }
                        // Log the full overlay list after all mutations are applied so we
                        // can verify the skin tone tile name is in the render chain.
                        std::string ov_log;
                        for( const auto &ov : host_npc->get_overlay_ids() ) {
                            ov_log += ov.first + ' ';
                        }
                        mp_log( "[cdda-mp] host_npc overlays after appearance: [" + ov_log + "]" );
                    }
                    // Apply or clear the host's wielded weapon.
                    if( incoming_wielded.empty() ) {
                        host_npc->remove_weapon();
                    } else {
                        const itype_id wid( incoming_wielded );
                        if( wid.is_valid() ) {
                            host_npc->set_wielded_item( item( wid ) );
                            mp_log( "[cdda-mp] host_wielded applied: " + incoming_wielded );
                        }
                    }
                }
                std::cout << " ok" << std::endl;
            }
        }

        // Apply host's move_mode to the host NPC proxy every tick (changes per-action).
        if( jo.has_string( "host_move_mode" ) && client_host_npc_spawned ) {
            npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
            if( host_npc ) {
                const move_mode_id mode_id( jo.get_string( "host_move_mode" ) );
                if( mode_id.is_valid() ) {
                    host_npc->move_mode = mode_id;
                }
            }
        }

        // Sync host facing direction for correct sprite flip.
        if( jo.has_int( "host_facing" ) && client_host_npc_spawned ) {
            npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
            if( host_npc ) {
                host_npc->facing = jo.get_int( "host_facing" ) == 0
                                   ? FacingDirection::LEFT : FacingDirection::RIGHT;
            }
        }

        std::cout << "[cdda-mp] monster sync..." << std::flush;
        apply_monster_sync( jo );
        std::cout << " ok" << std::endl;

        std::cout << "[cdda-mp] tile sync..." << std::flush;
        apply_tile_changes( jo );
        std::cout << " ok" << std::endl;

        std::cout << "[cdda-mp] vehicle sync..." << std::flush;
        apply_vehicle_sync( jo );
        std::cout << " ok" << std::endl;

        // Apply per-bodypart HP to the client avatar so the sidebar stays accurate.
        // Also synthesise "you were hit" messages from HP deltas.
        if( jo.has_array( "bodyparts" ) ) {
            avatar &av = get_avatar();
            int total_damage = 0;
            for( const JsonValue &bpv : jo.get_array( "bodyparts" ) ) {
                JsonObject bpo = bpv.get_object();
                bpo.allow_omitted_members();
                const std::string bp_str = bpo.get_string( "id" );
                const bodypart_id bp = bodypart_str_id( bp_str ).id();
                const int new_hp = bpo.get_int( "hp" );
                const auto prev_it = g_last_bodypart_hp.find( bp_str );
                if( prev_it != g_last_bodypart_hp.end() && new_hp < prev_it->second ) {
                    total_damage += prev_it->second - new_hp;
                }
                g_last_bodypart_hp[bp_str] = new_hp;
                av.set_part_hp_cur( bp, new_hp );
            }
            if( total_damage > 0 ) {
                add_msg( m_bad, "You are hit for %d damage!", total_damage );
            }
        }

        // "free":true means the server rejected the action as a no-op (e.g. wall bump).
        // Clear the ack guard immediately so the restored moves value is accepted below.
        if( jo.has_bool( "free" ) && jo.get_bool( "free" ) ) {
            g_client_waiting_for_ack = false;
        }

        // Apply server-authoritative move budget.
        // While g_client_waiting_for_ack is set (we sent an action but haven't
        // received the server's moves=0 ack yet), ignore moves>0 packets —
        // those are stale pre-ack broadcasts still in the TCP buffer.
        // Safety: if the ack hasn't arrived within 5 s (e.g. reconnect with stale
        // flag, or server timeout path that never sends moves<=0), force-clear it.
        if( g_client_waiting_for_ack ) {
            using namespace std::chrono_literals;
            if( std::chrono::steady_clock::now() - g_ack_set_time > 5s ) {
                mp_log( "[cdda-mp] ack guard timed out — force-clearing" );
                g_client_waiting_for_ack = false;
            }
        }
        if( jo.has_member( "moves" ) ) {
            const int srv_moves = jo.get_int( "moves" );
            if( srv_moves <= 0 ) {
                // Server locked us — action was received and processed.
                mp_log( "[cdda-mp] grant recv: moves=" + std::to_string( srv_moves ) +
                        " (ACK — clearing ack guard)" );
                g_client_waiting_for_ack = false;
                get_avatar().set_moves( srv_moves );
            } else if( !g_client_waiting_for_ack ) {
                // New grant and no unacked action in flight — apply normally.
                mp_log( "[cdda-mp] grant recv: moves=" + std::to_string( srv_moves ) +
                        " (applied)" );
                get_avatar().set_moves( srv_moves );
                g_last_grant_time = std::chrono::steady_clock::now();
            } else {
                // srv_moves > 0 but waiting for ack → stale packet, skip.
                mp_log( "[cdda-mp] grant recv: moves=" + std::to_string( srv_moves ) +
                        " (SKIPPED — ack pending)" );
            }
        }

        // Handle "keep smashing" continuation based on server bash result.
        if( jo.has_string( "smash_result" ) ) {
            const std::string sr = jo.get_string( "smash_result" );
            // Play the bash animation on the client if the server sent target coords.
            if( jo.has_int( "smash_x" ) && jo.has_int( "smash_y" ) && jo.has_int( "smash_z" ) ) {
                const tripoint_abs_ms abs_smash{
                    jo.get_int( "smash_x" ), jo.get_int( "smash_y" ), jo.get_int( "smash_z" )
                };
                map &here = get_map();
                if( here.inbounds( abs_smash ) ) {
                    const tripoint_bub_ms bub = here.get_bub( abs_smash );
                    if( sr == "destroyed" ) {
                        g->draw_async_anim( bub, "bash_complete", "X", c_light_gray );
                    } else if( sr == "hit" ) {
                        g->draw_async_anim( bub, "bash_effective", "/", c_light_gray );
                    } else if( sr == "impossible" ) {
                        g->draw_async_anim( bub, "bash_ineffective" );
                    }
                }
            }
            if( sr == "hit" ) {
                // Partial bash: ask once if not already in auto-smash mode, then re-queue.
                if( !g_client_autosmash ) {
                    g_client_autosmash = query_yn( _( "Keep smashing until destroyed?" ) );
                }
                if( g_client_autosmash && !g_client_autosmash_json.empty() ) {
                    g_pending_action = g_client_autosmash_json;
                }
            } else {
                // Destroyed, impossible, or failed — stop auto-smashing.
                g_client_autosmash = false;
                g_client_autosmash_json.clear();
            }
        }

        // Sync authoritative client stats from the server's NPC proxy.
        // The server is the source of truth for stamina, move_mode, and vehicle control —
        // it drains proxy stamina during movement and manages controlling_vehicle.
        g_client_ctrl_veh = jo.get_bool( "client_ctrl_veh", false );
        if( jo.has_object( "client_veh_pos" ) ) {
            JsonObject cvp = jo.get_object( "client_veh_pos" );
            cvp.allow_omitted_members();
            g_client_ctrl_veh_abs = tripoint_abs_ms{
                cvp.get_int( "x" ), cvp.get_int( "y" ), cvp.get_int( "z" )
            };
        } else {
            g_client_ctrl_veh_abs = tripoint_abs_ms{ 0, 0, 0 };
        }
        if( jo.has_string( "client_move_mode" ) ) {
            const move_mode_id mode_id( jo.get_string( "client_move_mode" ) );
            get_avatar().move_mode = mode_id;
            // Do NOT cancel desired_move_mode here — that would nuke a pending run/crouch
            // toggle the player just pressed before this state packet arrived.
            // The commit in handle_action already calls cancel_desired when set_movement_mode
            // fails (e.g. on stamina exhaustion), so no-op cancellation is redundant.
        }
        // Display forwarded combat messages from the host (hits, misses, kills).
        if( jo.has_array( "msgs" ) ) {
            for( const JsonValue &mv : jo.get_array( "msgs" ) ) {
                const std::string txt = mv.get_string();
                mp_log( "[cdda-mp] client recv msg: " + txt );
                add_msg( m_neutral, txt );
            }
        }

        // Play sfx events forwarded from the host's turn.
        if( jo.has_array( "sfx" ) ) {
            for( const JsonValue &sv : jo.get_array( "sfx" ) ) {
                JsonObject so = sv.get_object();
                so.allow_omitted_members();
                if( so.has_string( "id" ) && so.has_string( "v" ) && so.has_int( "vol" ) ) {
                    sfx::play_variant_sound( so.get_string( "id" ), so.get_string( "v" ),
                                            so.get_int( "vol" ) );
                }
            }
        }

        // Client manages its own inventory via the normal pickup dialog.
        // No server-driven inventory sync needed.

        std::cout << "[cdda-mp] state applied ok" << std::endl;

    } catch( const std::exception &e ) {
        std::cout << "[cdda-mp] exception in state processing: " << e.what() << std::endl;
    } catch( ... ) {
        std::cout << "[cdda-mp] unknown exception in state processing" << std::endl;
    }
    return true;
}

void client_process_incoming()
{
    // After the host has died (or the socket dropped), drain the queue silently
    // and do nothing else — prevents the lost-connection spam.
    if( g_server_died ) {
        std::string msg;
        while( client_recv_pop( msg ) ) {}
        return;
    }

    mp_cleanup_stale_npcs();

    // Send the join message on the first tick — the save is loaded by now.
    const bool was_joined = client_join_is_sent();
    client_send_join();
    if( !was_joined && client_join_is_sent() ) {
        // Just sent the join — clear any stale ack guard from a previous session
        // so the server's first move grant isn't silently ignored after reconnect.
        g_client_waiting_for_ack = false;
        // Immediately follow with our worn-item list and skin tone.
        client_resync_worn();
    }
    std::string msg;
    while( client_recv_pop( msg ) ) {
        apply_one_state_message( msg );
    }
    // Auto-fire any queued action now that the server has restored our moves.
    // Do NOT zero moves after firing — leave moves > 0 so the input loop runs
    // immediately after, giving the user the chance to queue the next action.
    // The ack guard (set below) prevents the input loop from double-sending.
    if( !g_pending_action.empty() && get_avatar().get_moves() > 0 ) {
        mp_log( "[cdda-mp] auto-fire: pending=" + g_pending_action.substr( 0, 60 ) );
        // If we're now in a wait activity, discard the stale queued action instead
        // of sending it — catching breath / ACT_WAIT should not dispatch a move.
        const player_activity &pact = get_avatar().activity;
        static const activity_id act_wait( "ACT_WAIT" );
        static const activity_id act_wait_stamina( "ACT_WAIT_STAMINA" );
        static const activity_id act_wait_weather( "ACT_WAIT_WEATHER" );
        static const activity_id act_wait_npc( "ACT_WAIT_NPC" );
        if( pact && ( pact.id() == act_wait || pact.id() == act_wait_stamina ||
                      pact.id() == act_wait_weather || pact.id() == act_wait_npc ) ) {
            mp_log( "[cdda-mp] auto-fire suppressed: in wait activity " + pact.id().str() );
            g_pending_action.clear();
            // Leave moves intact — activity loop will consume them below.
        } else {
            mp_log( "[cdda-mp] auto-fire: SENDING" );
            client_send( g_pending_action );
            g_pending_action.clear();
            // Keep moves > 0: input loop will run so the user can queue the next action.
            // Ack guard prevents a second send before the server acknowledges this one.
            g_client_waiting_for_ack = true;
            g_ack_set_time = std::chrono::steady_clock::now();
        }
    }
    // Warn if the client is drifting too far from the host's reality bubble center.
    if( client_host_npc_id.is_valid() ) {
        npc *hnpc = g->critter_by_id<npc>( client_host_npc_id );
        if( hnpc ) {
            check_separation_warning( get_avatar().pos_abs(), hnpc->pos_abs() );
        }
    }
}

// Scan tiles around the client avatar for field changes (blood, etc.) since the
// last action was sent.  Returns a JSON array of changed tile entries suitable
// for inclusion as "client_tile_changes" in an action packet.
static std::string build_client_tile_changes( int radius = 10 )
{
    const avatar &av = get_avatar();
    const tripoint_abs_ms center = av.pos_abs();
    map &m = get_map();
    std::string out = "[";
    bool first = true;

    for( int dy = -radius; dy <= radius; ++dy ) {
        for( int dx = -radius; dx <= radius; ++dx ) {
            const tripoint_abs_ms abs{ center.x() + dx, center.y() + dy, center.z() };
            if( !m.inbounds( abs ) ) {
                continue;
            }
            const tripoint_bub_ms bub = m.get_bub( abs );

            // Terrain + furniture — baseline-gated.
            const std::string ter_str  = m.ter( bub ).id().str();
            const std::string furn_str = m.furn( bub ).id().str();
            const std::string terfurn_sig = ter_str + '|' + furn_str;
            auto &terfurn_baseline = g_client_terfurn_baseline[abs];
            const bool terfurn_changed = ( terfurn_baseline != terfurn_sig );
            if( terfurn_changed ) {
                terfurn_baseline = terfurn_sig;
            }

            // Items — baseline-gated so we only send when the tile changes.
            // Full item serialize() is used so nested pocket contents are included.
            std::string items_sig;
            std::string items_json = "[]";
            auto items = m.i_at( bub );
            if( !items.empty() ) {
                items_json = "[";
                bool ifirst = true;
                for( const item &it : items ) {
                    const std::string item_json = serialize( it );
                    items_sig += item_json + ',';
                    if( !ifirst ) {
                        items_json += ',';
                    }
                    ifirst = false;
                    items_json += item_json;
                }
                items_json += "]";
            }
            auto &item_baseline = g_client_item_baseline[abs];
            const bool items_changed = ( item_baseline != items_sig );
            if( items_changed ) {
                item_baseline = items_sig;
                if( !items_sig.empty() ) {
                    mp_log( "[cdda-mp] client tile items @ " +
                            std::to_string( abs.x() ) + "," +
                            std::to_string( abs.y() ) + "," +
                            std::to_string( abs.z() ) );
                }
            }

            // Fields — always re-sent every turn because they decay on the server.
            std::string fields_sig;
            std::string fields_json = "[]";
            const field &fld = m.field_at( bub );
            if( fld.field_count() > 0 ) {
                fields_json = "[";
                bool ffield = true;
                for( const auto &[ftype, fentry] : fld ) {
                    if( !fentry.is_field_alive() ) {
                        continue;
                    }
                    const int fi = fentry.get_field_intensity();
                    fields_sig += ftype.id().str() + ':' + std::to_string( fi ) + ',';
                    if( !ffield ) {
                        fields_json += ',';
                    }
                    ffield = false;
                    fields_json += "{\"t\":\"" + ftype.id().str()
                                   + "\",\"i\":" + std::to_string( fi ) + "}";
                }
                fields_json += "]";
            }

            // Trap — baseline-gated.
            const trap &tr_here = m.tr_at( bub );
            const std::string trap_sig_c = tr_here.is_null() ? "" : tr_here.id.str();
            auto &trap_baseline = g_client_trap_baseline[abs];
            const bool trap_changed = ( trap_baseline != trap_sig_c );
            if( trap_changed ) {
                trap_baseline = trap_sig_c;
            }

            // Graffiti — baseline-gated.
            const std::string graffiti_sig_c = m.has_graffiti_at( bub ) ? m.graffiti_at( bub ) : "";
            auto &graffiti_baseline = g_client_graffiti_baseline[abs];
            const bool graffiti_changed = ( graffiti_baseline != graffiti_sig_c );
            if( graffiti_changed ) {
                graffiti_baseline = graffiti_sig_c;
            }

            if( !terfurn_changed && !items_changed && fields_sig.empty() &&
                !trap_changed && !graffiti_changed ) {
                continue;
            }

            if( !first ) {
                out += ',';
            }
            first = false;
            out += "{\"x\":" + std::to_string( abs.x() )
                   + ",\"y\":" + std::to_string( abs.y() )
                   + ",\"z\":" + std::to_string( abs.z() );
            if( terfurn_changed ) {
                out += ",\"ter\":\"" + ter_str + "\",\"furn\":\"" + furn_str + "\"";
            }
            if( items_changed ) {
                out += ",\"items\":" + items_json;
            }
            if( !fields_sig.empty() ) {
                out += ",\"fields\":" + fields_json;
            }
            if( trap_changed ) {
                out += ",\"trap\":\"" + ( trap_sig_c.empty() ? std::string( "tr_null" ) : trap_sig_c ) + "\"";
            }
            if( graffiti_changed ) {
                out += ",\"graffiti\":\"" + json_escape_str( graffiti_sig_c ) + "\"";
            }
            out += "}";
        }
    }
    out += ']';
    return out;
}

// Build JSON array of monsters the client damaged since the last server sync.
// Uses g_last_monster_hp (last server-reported HP) as the baseline.
static std::string build_client_monster_hits()
{
    std::string hits;
    bool first = true;
    for( const auto &ptr : get_creature_tracker().get_monsters_list() ) {
        monster *mon = ptr.get();
        if( !mon || mon->mp_net_id == 0 ) {
            continue;
        }
        const auto it = g_last_monster_hp.find( mon->mp_net_id );
        if( it == g_last_monster_hp.end() ) {
            continue;
        }
        const int client_hp = mon->is_dead() ? 0 : mon->get_hp();
        if( client_hp >= it->second ) {
            continue;
        }
        if( !first ) { hits += ','; }
        first = false;
        hits += "{\"nid\":" + std::to_string( mon->mp_net_id )
             + ",\"hp\":" + std::to_string( client_hp ) + "}";
    }
    return first ? std::string() : ( "[" + hits + "]" );
}

std::string client_enrich_action( const std::string &json )
{
    const avatar &av = get_avatar();

    std::string bleed_json = "[";
    bool bleed_first = true;
    for( const bodypart_id &bp : av.get_all_body_parts() ) {
        const int intensity = av.get_effect_int( effect_bleed, bp );
        if( intensity > 0 ) {
            if( !bleed_first ) {
                bleed_json += ',';
            }
            bleed_first = false;
            bleed_json += "{\"bp\":\"" + bp.id().str() +
                          "\",\"intensity\":" + std::to_string( intensity ) + "}";
        }
    }
    bleed_json += "]";

    const float cl = av.active_light();
    const std::string tile_changes = build_client_tile_changes();

    // Build char_stats block: base stats, all skills, and known proficiencies.
    // Applied server-side to the NPC proxy so pldrive(), melee, etc. use real values.
    std::string char_stats = "{";
    char_stats += "\"str\":" + std::to_string( av.get_str_base() );
    char_stats += ",\"dex\":" + std::to_string( av.get_dex_base() );
    char_stats += ",\"int\":" + std::to_string( av.get_int_base() );
    char_stats += ",\"per\":" + std::to_string( av.get_per_base() );
    char_stats += ",\"skills\":[";
    bool first_s = true;
    for( const auto &[sid, slevel] : av.get_all_skills() ) {
        const int lvl = slevel.level();
        if( lvl <= 0 ) {
            continue;
        }
        if( !first_s ) {
            char_stats += ',';
        }
        first_s = false;
        char_stats += "[\"" + sid.str() + "\"," + std::to_string( lvl ) + "]";
    }
    char_stats += "],\"profs\":[";
    bool first_p = true;
    for( const proficiency_id &pid : av.known_proficiencies() ) {
        if( !first_p ) {
            char_stats += ',';
        }
        first_p = false;
        char_stats += "\"" + pid.str() + "\"";
    }
    char_stats += "]}";

    std::string enriched = json;
    if( !enriched.empty() && enriched.back() == '}' ) {
        enriched.pop_back();
        enriched += ",\"client_light\":" + std::to_string( cl );
        enriched += ",\"client_bleed\":" + bleed_json;
        enriched += ",\"client_tile_changes\":" + tile_changes;
        enriched += ",\"client_stamina\":" + std::to_string( av.get_stamina() );
        const std::string monster_hits = build_client_monster_hits();
        if( !monster_hits.empty() ) {
            enriched += ",\"client_monster_hits\":" + monster_hits;
        }
        enriched += ",\"char_stats\":" + char_stats;
        enriched += ",\"client_facing\":" + std::to_string(
                        av.facing == FacingDirection::LEFT ? 0 : 1 );
        enriched += '}';
    }
    return enriched;
}

void client_queue_action( const std::string &json )
{
    g_pending_action = client_enrich_action( json );
}

float get_host_luminance()
{
    return g_mp_host_luminance;
}

float get_remote_player_luminance()
{
    return g_mp_remote_player_luminance;
}

character_id get_host_npc_character_id()
{
    return client_host_npc_id;
}

character_id get_remote_player_npc_character_id()
{
    return remote_player_npc_id;
}

void client_resync_worn()
{
    avatar &av = get_avatar();
    std::vector<item *> worn_items;
    av.worn.inv_dump( worn_items );

    // Collect all appearance-related mutations by type and bundle them into a
    // JSON array so the server NPC proxy renders with the correct visual overlays.
    // hair_style is special: it can carry a color variant; all others are plain ids.
    static const std::vector<std::string> simple_appearance_types = {
        "skin_tone", "eye_color", "facial_hair", "hair_color"
    };
    std::string appearance_json = "[";
    bool afirst = true;
    // Simple types: just the trait id, no variant.
    for( const std::string &mtype : simple_appearance_types ) {
        for( const trait_id &tid : get_mutations_in_type( mtype ) ) {
            if( av.has_trait( tid ) ) {
                if( !afirst ) { appearance_json += ','; }
                afirst = false;
                appearance_json += "{\"type\":\"" + mtype
                                 + "\",\"id\":\"" + tid.str() + "\"}";
                break; // at most one active per type
            }
        }
    }
    // hair_style: include the color variant so the proxy gets the right color.
    for( const trait_and_var &tv : av.get_mutations_variants() ) {
        if( tv.trait.obj().types.count( "hair_style" ) ) {
            if( !afirst ) { appearance_json += ','; }
            afirst = false;
            appearance_json += "{\"type\":\"hair_style\",\"id\":\"" + tv.trait.str() + "\"";
            if( !tv.variant.empty() ) {
                appearance_json += ",\"var\":\"" + tv.variant + "\"";
            }
            appearance_json += '}';
            break;
        }
    }
    appearance_json += ']';

    std::string worn_json = "{\"action\":\"worn_sync\",\"worn\":[";
    bool wfirst = true;
    for( const item *it : worn_items ) {
        if( !wfirst ) {
            worn_json += ',';
        }
        wfirst = false;
        worn_json += "{\"t\":\"" + it->typeId().str() + "\"";
        if( it->has_itype_variant() ) {
            worn_json += ",\"v\":\"" + it->itype_variant().id + "\"";
        }
        worn_json += "}";
    }
    std::string wielded_type;
    item_location wielded = av.get_wielded_item();
    if( wielded ) {
        wielded_type = wielded->typeId().str();
    }
    const std::string male_str = av.male ? "true" : "false";
    worn_json += "],\"male\":" + male_str
                 + ",\"appearance\":" + appearance_json
                 + ",\"wielded\":\"" + wielded_type + "\"}";
    client_send( worn_json );
}

void client_mark_action_sent()
{
    g_client_waiting_for_ack = true;
    g_ack_set_time = std::chrono::steady_clock::now();
}

bool is_client_waiting_for_ack()
{
    return g_client_waiting_for_ack;
}

void mp_client_post_action( int pre_moves )
{
    if( !is_client_mode() ) {
        return;
    }
    if( get_avatar().get_moves() >= pre_moves ) {
        return;
    }
    client_resync_worn();
    client_send( client_enrich_action( "{\"type\":\"action\",\"action\":\"wait\"}" ) );
    get_avatar().set_moves( 0 );
    client_mark_action_sent();
}

int ms_since_last_grant()
{
    using namespace std::chrono;
    return static_cast<int>(
        duration_cast<milliseconds>( steady_clock::now() - g_last_grant_time ).count() );
}

bool client_ctrl_veh()
{
    return g_client_ctrl_veh;
}

void set_client_ctrl_veh( bool b )
{
    g_client_ctrl_veh = b;
    if( !b ) {
        g_client_ctrl_veh_abs = tripoint_abs_ms{ 0, 0, 0 };
    }
}

tripoint_abs_ms client_ctrl_veh_abs()
{
    return g_client_ctrl_veh_abs;
}

void client_set_autosmash_json( const std::string &json )
{
    // Clear any existing auto-smash state so a new manual smash starts fresh.
    g_client_autosmash = false;
    g_client_autosmash_json = json;
}

void client_dispatch_wait_for_activity( const activity_id &pre_id, bool force_idle )
{
    if( g_client_waiting_for_ack ) {
        mp_log( "[cdda-mp] dispatch_wait: ack pending, skip" );
        return;
    }
    const player_activity &pact = get_avatar().activity;
    const activity_id &id = pact ? pact.id() : pre_id;
    if( !id && !force_idle ) {
        mp_log( "[cdda-mp] dispatch_wait: no activity, skip" );
        return;
    }
    mp_log( "[cdda-mp] dispatch_wait: SEND wait for act=" + ( id ? id.str() : "idle" ) );
    client_send( client_enrich_action( "{\"type\":\"action\",\"action\":\"wait\"}" ) );
    client_mark_action_sent();
}

// Server: scan the sync area and emit tile entries whose ter/furn/items changed since last broadcast.
static std::string build_tile_changes( const tripoint_abs_ms &center, int radius )
{
    std::string out = "[";
    bool first = true;
    map &m = get_map();

    for( int dy = -radius; dy <= radius; ++dy ) {
        for( int dx = -radius; dx <= radius; ++dx ) {
            const tripoint_abs_ms abs{ center.x() + dx, center.y() + dy, center.z() };
            if( !m.inbounds( abs ) ) {
                continue;
            }
            const tripoint_bub_ms bub = m.get_bub( abs );
            const std::string ter_str  = m.ter( bub ).id().str();
            const std::string furn_str = m.furn( bub ).id().str();

            // Build item fingerprint and JSON simultaneously.
            // Full item serialize() is used so nested pocket contents are included.
            std::string items_sig;
            std::string items_json = "[]";
            auto items = m.i_at( bub );
            if( !items.empty() ) {
                items_json = "[";
                bool ifirst = true;
                for( const item &it : items ) {
                    const std::string item_json = serialize( it );
                    items_sig += item_json + ',';
                    if( !ifirst ) {
                        items_json += ',';
                    }
                    ifirst = false;
                    items_json += item_json;
                }
                items_json += "]";
            }

            // Build field fingerprint and JSON.
            std::string fields_sig;
            std::string fields_json = "[]";
            const field &fld = m.field_at( bub );
            if( fld.field_count() > 0 ) {
                fields_json = "[";
                bool ffield = true;
                for( const auto &[ftype, fentry] : fld ) {
                    if( !fentry.is_field_alive() ) {
                        continue;
                    }
                    const int fi = fentry.get_field_intensity();
                    fields_sig += ftype.id().str() + ':' + std::to_string( fi ) + ',';
                    if( !ffield ) {
                        fields_json += ',';
                    }
                    ffield = false;
                    fields_json += "{\"t\":\"" + ftype.id().str()
                                   + "\",\"i\":" + std::to_string( fi ) + "}";
                }
                fields_json += "]";
            }

            // Trap — id string, empty when no placed trap (tr_null).
            const trap &tr      = m.tr_at( bub );
            const std::string trap_sig = tr.is_null() ? "" : tr.id.str();

            // Graffiti.
            const std::string graffiti_sig = m.has_graffiti_at( bub ) ? m.graffiti_at( bub ) : "";

            auto &baseline = g_tile_baseline[abs];
            if( baseline.ter == ter_str && baseline.furn == furn_str &&
                baseline.items_sig == items_sig && baseline.fields_sig == fields_sig &&
                baseline.trap_sig == trap_sig && baseline.graffiti_sig == graffiti_sig ) {
                continue; // Nothing changed — skip this tile.
            }
            baseline.ter          = ter_str;
            baseline.furn         = furn_str;
            baseline.items_sig    = items_sig;
            baseline.fields_sig   = fields_sig;
            baseline.trap_sig     = trap_sig;
            baseline.graffiti_sig = graffiti_sig;

            if( !items_sig.empty() ) {
                mp_log( "tile_delta items @ " +
                        std::to_string( abs.x() ) + "," +
                        std::to_string( abs.y() ) + "," +
                        std::to_string( abs.z() ) );
            }
            if( !fields_sig.empty() ) {
                mp_log( "tile_delta fields @ " +
                        std::to_string( abs.x() ) + "," +
                        std::to_string( abs.y() ) + "," +
                        std::to_string( abs.z() ) + " : " + fields_sig );
            }

            if( !first ) {
                out += ',';
            }
            first = false;
            out += "{\"x\":" + std::to_string( abs.x() )
                   + ",\"y\":" + std::to_string( abs.y() )
                   + ",\"z\":" + std::to_string( abs.z() )
                   + ",\"ter\":\"" + ter_str + "\""
                   + ",\"furn\":\"" + furn_str + "\""
                   + ",\"items\":" + items_json
                   + ",\"fields\":" + fields_json
                   + ",\"trap\":\"" + ( trap_sig.empty() ? "tr_null" : trap_sig ) + "\""
                   + ",\"graffiti\":\"" + json_escape_str( graffiti_sig ) + "\""
                   + "}";
        }
    }
    out += ']';
    return out;
}

static std::string build_monster_list( const tripoint_abs_ms &center, int radius )
{
    std::string out = "[";
    bool first = true;
    for( const auto &mon_ptr : get_creature_tracker().get_monsters_list() ) {
        if( !mon_ptr ) {
            continue;
        }
        const tripoint_abs_ms mp = mon_ptr->pos_abs();
        if( std::abs( mp.x() - center.x() ) > radius ||
            std::abs( mp.y() - center.y() ) > radius ||
            mp.z() != center.z() ) {
            continue;
        }
        // Assign a stable network ID the first time this monster enters sync range.
        if( mon_ptr->mp_net_id == 0 ) {
            mon_ptr->mp_net_id = ++g_next_net_id;
        }
        if( !first ) {
            out += ',';
        }
        first = false;
        const int mon_facing = ( mon_ptr->facing == FacingDirection::LEFT ) ? 0 : 1;
        out += "{\"nid\":" + std::to_string( mon_ptr->mp_net_id )
               + ",\"id\":\"" + mon_ptr->type->id.str() + "\""
               + ",\"x\":" + std::to_string( mp.x() )
               + ",\"y\":" + std::to_string( mp.y() )
               + ",\"z\":" + std::to_string( mp.z() )
               + ",\"hp\":" + std::to_string( mon_ptr->get_hp() )
               + ",\"facing\":" + std::to_string( mon_facing ) + "}";
    }
    out += ']';
    return out;
}

// Client: apply terrain, furniture, and item changes received from the server.
static void apply_tile_changes( JsonObject &jo )
{
    if( !jo.has_array( "tile_changes" ) ) {
        return;
    }
    map &m = get_map();
    bool any_new_trap = false;

    for( const JsonValue &entry : jo.get_array( "tile_changes" ) ) {
        JsonObject to = entry.get_object();
        to.allow_omitted_members();

        const tripoint_abs_ms abs{
            to.get_int( "x" ), to.get_int( "y" ), to.get_int( "z" )
        };
        if( !m.inbounds( abs ) ) {
            continue;
        }
        const tripoint_bub_ms bub = m.get_bub( abs );

        std::string ter_str, furn_str;
        to.read( "ter", ter_str );
        to.read( "furn", furn_str );

        if( !ter_str.empty() ) {
            m.ter_set( bub, ter_id( ter_str ) );
        }
        if( !furn_str.empty() ) {
            m.furn_set( bub, furn_id( furn_str ) );
        }

        if( to.has_array( "items" ) ) {
            // Log any items that currently exist locally but are about to be
            // cleared — these are client-only drops that the server doesn't know
            // about and will erase.
            auto existing = m.i_at( bub );
            if( !existing.empty() ) {
                std::string had;
                for( const item &it : existing ) {
                    had += it.typeId().str() + ' ';
                }
                mp_log( "[cdda-mp] apply_tile_changes: clearing local items @ " +
                        std::to_string( abs.x() ) + "," +
                        std::to_string( abs.y() ) + "," +
                        std::to_string( abs.z() ) + " had=[" + had + "]" );
            }
            m.i_clear( bub );
            std::string applied;
            for( const JsonValue &iv : to.get_array( "items" ) ) {
                try {
                    item new_item;
                    JsonObject io = iv.get_object();
                    io.allow_omitted_members();
                    new_item.deserialize( io );
                    if( !new_item.typeId().is_empty() && new_item.typeId().is_valid() ) {
                        applied += new_item.typeId().str() + ' ';
                        m.add_item( bub, std::move( new_item ) );
                    }
                } catch( const JsonError & ) {}
            }
            if( !applied.empty() ) {
                mp_log( "[cdda-mp] apply_tile_changes: set items @ " +
                        std::to_string( abs.x() ) + "," +
                        std::to_string( abs.y() ) + "," +
                        std::to_string( abs.z() ) + " items=[" + applied + "]" );
            }
        }

        if( to.has_array( "fields" ) ) {
            // Clear all existing fields on this tile, then apply the server's set.
            field &fld = m.field_at( bub );
            std::vector<field_type_id> to_remove;
            for( const auto &[ftype, fentry] : fld ) {
                to_remove.push_back( ftype );
            }
            for( const field_type_id &ftype : to_remove ) {
                m.delete_field( bub, ftype );
            }
            for( const JsonValue &fv : to.get_array( "fields" ) ) {
                JsonObject fo = fv.get_object();
                fo.allow_omitted_members();
                const std::string type_str = fo.get_string( "t", "" );
                if( type_str.empty() ) {
                    continue;
                }
                const field_type_id ftid( type_str );
                if( ftid.is_valid() ) {
                    m.add_field( bub, ftid, fo.get_int( "i", 1 ) );
                }
            }
        }

        if( to.has_string( "trap" ) ) {
            const std::string trap_str = to.get_string( "trap" );
            if( trap_str.empty() || trap_str == "tr_null" ) {
                m.trap_set( bub, tr_null );
            } else {
                const trap_str_id tsid( trap_str );
                if( tsid.is_valid() ) {
                    m.trap_set( bub, tsid.id() );
                    any_new_trap = true;
                }
            }
        }

        if( to.has_string( "graffiti" ) ) {
            const std::string gtext = to.get_string( "graffiti" );
            if( gtext.empty() ) {
                m.delete_graffiti( bub );
            } else {
                m.set_graffiti( bub, gtext );
            }
        }
    }

    // Run detection so newly synced traps show the warning tile immediately,
    // mirroring the search_surroundings() call that SP makes after every move.
    if( any_new_trap ) {
        get_avatar().search_surroundings();
    }
}

// Client: apply vehicle position, facing, and velocity from the server state packet.
// Scope: stationary and moving vehicles visible to the host.  Driving, boarding,
// and reality-bubble edge transitions are excluded until those features are designed.
static void apply_vehicle_sync( JsonObject &jo )
{
    if( !jo.has_array( "vehicles" ) ) {
        return;
    }
    map &m = get_map();
    const VehicleList vehs = m.get_vehicles();

    for( const JsonValue &entry : jo.get_array( "vehicles" ) ) {
        JsonObject vo = entry.get_object();
        vo.allow_omitted_members();

        const auto nid       = static_cast<uint32_t>( vo.get_int( "nid", 0 ) );
        const tripoint_abs_ms new_abs{
            vo.get_int( "x" ), vo.get_int( "y" ), vo.get_int( "z" )
        };
        const int  face_deg     = vo.get_int( "face", 0 );
        const int  turn_dir_deg = vo.get_int( "turn_dir", face_deg );
        const int  vel          = vo.get_int( "vel",  0 );
        const int  cruise       = vo.get_int( "cruise", vel );
        // Read name early — used as fallback identifier when position lookups fail
        // (e.g. the client's local physics have drifted the vehicle away from both
        // the tracked position and the server's authoritative position).
        std::string vname;
        vo.read( "name", vname );

        // Find the vehicle object.  Primary: look at last-known abs position.
        vehicle *found = nullptr;
        auto pos_it = g_client_veh_pos.find( nid );
        const bool first_encounter = ( pos_it == g_client_veh_pos.end() );
        const tripoint_abs_ms search_abs = ( pos_it != g_client_veh_pos.end() )
                                           ? pos_it->second
                                           : new_abs;

        if( m.inbounds( search_abs ) ) {
            for( const wrapped_vehicle &wv : vehs ) {
                if( wv.v && wv.v->pos_abs() == search_abs ) {
                    found = wv.v;
                    break;
                }
            }
        }
        // Fallback: server's authoritative position (first packet, or vehicle there).
        if( !found && m.inbounds( new_abs ) ) {
            for( const wrapped_vehicle &wv : vehs ) {
                if( wv.v && wv.v->pos_abs() == new_abs ) {
                    found = wv.v;
                    break;
                }
            }
        }
        // Name fallback: client physics may have drifted the vehicle away from both
        // the tracked position and new_abs.  Match by name so we can snap it back.
        if( !found && !vname.empty() ) {
            for( const wrapped_vehicle &wv : vehs ) {
                if( wv.v && wv.v->name == vname ) {
                    found = wv.v;
                    break;
                }
            }
        }

        if( !found ) {
            continue;
        }

        // Compute target facing.
        const units::angle face_angle = units::from_degrees( face_deg );
        const bool face_changed = ( found->face.dir() != face_angle );

        // Always prime precalc[1] before any displacement.
        // displace_vehicle → advance_precalc_mounts copies precalc[1] → precalc[0]
        // then calls add_vehicle_to_cache.  Without this, a pure translation (no face
        // change) would copy stale precalc[1] into the cache — wrong part positions.
        found->pivot_rotation[1] = face_angle;
        found->precalc_mounts( 1, face_angle, found->pivot_point( m ) );

        // Move vehicle to server-authoritative position if it has changed AND the
        // target is inside the client's loaded map.  Driving out of the bubble is
        // handled naturally when the submaps unload; we don't force the vehicle to
        // follow into territory the client hasn't loaded.
        bool did_displace = false;
        if( found->pos_abs() != new_abs && m.inbounds( new_abs ) ) {
            const tripoint_bub_ms cur_bub = found->pos_bub( m );
            const tripoint_bub_ms new_bub = m.get_bub( new_abs );
            const tripoint_rel_ms dp      = new_bub - cur_bub;
            if( dp != tripoint_rel_ms::zero ) {
                m.displace_vehicle( *found, dp );
                did_displace = true;
                // advance_precalc_mounts inside displace_vehicle already:
                //   - cleared the rendering cache at old positions
                //   - copied precalc[1] (new facing) → precalc[0]
                //   - set pivot_rotation[0] = pivot_rotation[1] = face_angle
                // add_vehicle_to_cache was called inside displace_vehicle with the
                // correct new-facing part positions.
            }
        }

        // Update tracked position only after we've confirmed the vehicle exists
        // at a location the client can see.  This keeps the lookup anchor valid.
        g_client_veh_pos[nid] = found->pos_abs();

        // Finalise facing state.  face.init() and move.init() must still be updated
        // even when displace_vehicle already set pivot_rotation[0].
        // turn_dir synced unconditionally: it updates immediately after pldrive on the
        // server (before vehmove), so the client sees the new intended heading right away.
        found->turn_dir = units::from_degrees( turn_dir_deg );

        if( face_changed ) {
            found->face.init( face_angle );
            found->move.init( face_angle );
            found->pivot_rotation[0] = face_angle;

            if( !did_displace ) {
                // No displacement occurred: advance_precalc_mounts was NOT called,
                // so precalc[0] and the rendering cache still reflect the old facing.
                // Clear the stale cache entries, recompute precalc[0], then re-add.
                for( const vpart_reference &vpr : found->get_all_parts_with_fakes() ) {
                    if( !vpr.part().removed ) {
                        m.clear_vehicle_point_from_cache( found,
                                                          found->bub_part_pos( m, vpr.part() ) );
                    }
                }
                found->precalc_mounts( 0, face_angle, found->pivot_point( m ) );
                m.add_vehicle_to_cache( found );
            }
            // did_displace case: cache and precalc[0] are already correct (set by
            // advance_precalc_mounts from the pre-set precalc[1]).
        }

        // First encounter: vehicle may have uninitialized precalc (debug spawn, save
        // load without a physics tick, etc.).  Force a full rebuild so the render
        // cache reflects correct part positions from the start.
        if( first_encounter && !did_displace && !face_changed ) {
            for( const vpart_reference &vpr : found->get_all_parts_with_fakes() ) {
                if( !vpr.part().removed ) {
                    m.clear_vehicle_point_from_cache( found,
                                                      found->bub_part_pos( m, vpr.part() ) );
                }
            }
            found->precalc_mounts( 0, found->face.dir(), found->pivot_anchor[0] );
            m.add_vehicle_to_cache( found );
        }

        // Update velocity and cruise target (server is authoritative for physics).
        found->velocity = vel;
        found->cruise_velocity = cruise;

        // Sync vehicle name — server is authoritative; overrides any local rename.
        if( !vname.empty() && found->name != vname ) {
            found->name = vname;
        }

        // Apply per-part state (open/enabled).  Parts are indexed by sequential
        // position over get_all_parts() — same iteration order on both sides.
        if( vo.has_array( "parts" ) ) {
            // Build index → part pointer map using the same iteration as serialize.
            std::vector<vehicle_part *> part_ptrs;
            for( const vpart_reference &vpr : found->get_all_parts() ) {
                part_ptrs.push_back( &vpr.part() );
            }

            bool parts_changed = false;
            for( const JsonValue &pv : vo.get_array( "parts" ) ) {
                JsonObject po = const_cast<JsonValue &>( pv ).get_object();
                po.allow_omitted_members();
                const int idx = po.get_int( "i", -1 );
                if( idx < 0 || idx >= static_cast<int>( part_ptrs.size() ) ) {
                    continue;
                }
                vehicle_part &vp = *part_ptrs[idx];
                const bool new_open    = po.get_bool( "open",    vp.open );
                const bool new_enabled = po.get_bool( "enabled", vp.enabled );
                if( vp.open != new_open || vp.enabled != new_enabled ) {
                    vp.open    = new_open;
                    vp.enabled = new_enabled;
                    parts_changed = true;
                }
                // HP: server is authoritative; apply if it differs from local value.
                const int new_hp = po.get_int( "hp", -1 );
                if( new_hp >= 0 && new_hp != vp.hp() ) {
                    found->set_hp( vp, new_hp, true );
                    parts_changed = true;
                }
                // Fuel: sync amount for tanks and fuel stores.
                std::string fuel_type_str;
                if( po.read( "fuel_type", fuel_type_str ) && !fuel_type_str.empty() ) {
                    const itype_id ftype( fuel_type_str );
                    const int fuel_amt = po.get_int( "fuel_amt", 0 );
                    if( ftype.is_valid() && vp.ammo_remaining() != fuel_amt ) {
                        vp.ammo_set( ftype, fuel_amt );
                    }
                }
            }
            // Rebuild the render cache so part sprite changes are visible immediately.
            if( parts_changed ) {
                for( const vpart_reference &vpr : found->get_all_parts_with_fakes() ) {
                    if( !vpr.part().removed ) {
                        m.clear_vehicle_point_from_cache( found,
                                                          found->bub_part_pos( m, vpr.part() ) );
                    }
                }
                found->precalc_mounts( 0, found->face.dir(), found->pivot_anchor[0] );
                m.add_vehicle_to_cache( found );
            }
        }
    }
}

static void apply_monster_sync( JsonObject &jo )
{
    if( !jo.has_array( "monsters" ) ) {
        return;
    }

    creature_tracker &ct = get_creature_tracker();
    const std::vector<shared_ptr_fast<monster>> &mons = ct.get_monsters_list();
    map &m = get_map();

    // Rebuild net_id → monster* map from current creature_tracker state.
    g_net_id_map.clear();
    for( const auto &ptr : mons ) {
        if( ptr && ptr->mp_net_id != 0 ) {
            g_net_id_map[ptr->mp_net_id] = ptr.get();
        }
    }

    constexpr int SYNC_RADIUS = 40;
    const tripoint_abs_ms region_center = g_mp_remote_pos;

    // Collect all client monsters inside the sync region for cleanup pass.
    std::unordered_set<monster *> in_region;
    for( const auto &ptr : mons ) {
        monster *mon = ptr.get();
        if( !mon ) {
            continue;
        }
        const tripoint_abs_ms mp = mon->pos_abs();
        if( std::abs( mp.x() - region_center.x() ) <= SYNC_RADIUS &&
            std::abs( mp.y() - region_center.y() ) <= SYNC_RADIUS &&
            mp.z() == region_center.z() ) {
            in_region.insert( mon );
        }
    }

    std::unordered_set<monster *> matched;

    for( const JsonValue &entry : jo.get_array( "monsters" ) ) {
        JsonObject mo = entry.get_object();
        mo.allow_omitted_members();

        const auto nid       = static_cast<uint32_t>( mo.get_int( "nid", 0 ) );
        const int  server_hp = mo.get_int( "hp", -1 );
        const tripoint_abs_ms target{
            mo.get_int( "x" ), mo.get_int( "y" ), mo.get_int( "z" )
        };

        monster *best = nullptr;

        // --- Primary lookup: stable network ID ---
        if( nid != 0 ) {
            auto it = g_net_id_map.find( nid );
            if( it != g_net_id_map.end() && !matched.count( it->second ) ) {
                best = it->second;
            }
        }

        // --- Fallback: proximity + type for monsters not yet assigned an ID ---
        // Covers the initial connect where the client's local monsters have mp_net_id == 0.
        if( best == nullptr ) {
            std::string id_str;
            mo.read( "id", id_str );
            if( !id_str.empty() ) {
                const mtype_id mid( id_str );
                int best_dist = 15;
                for( const auto &ptr : mons ) {
                    monster *mon = ptr.get();
                    if( !mon || mon->type->id != mid || matched.count( mon ) ) {
                        continue;
                    }
                    if( mon->mp_net_id != 0 && mon->mp_net_id != nid ) {
                        continue;  // Already tracked under a different server ID
                    }
                    const tripoint_abs_ms mp = mon->pos_abs();
                    const int dist = std::abs( mp.x() - target.x() ) +
                                     std::abs( mp.y() - target.y() );
                    if( dist < best_dist ) {
                        best_dist = dist;
                        best = mon;
                    }
                }
                // Assign the server's ID so future lookups are O(1).
                if( best != nullptr && nid != 0 && best->mp_net_id == 0 ) {
                    best->mp_net_id = nid;
                    g_net_id_map[nid] = best;
                }
            }
        }

        // --- Spawn: server has a monster the client doesn't know about ---
        if( best == nullptr && m.inbounds( target ) && server_hp > 0 ) {
            std::string id_str;
            mo.read( "id", id_str );
            if( !id_str.empty() ) {
                const mtype_id mid( id_str );
                const tripoint_bub_ms bub = m.get_bub( target );
                auto new_mon = make_shared_fast<monster>( mid, bub );
                new_mon->mp_net_id = nid;
                new_mon->set_hp( server_hp );
                best = g->place_critter_at( new_mon, bub );
                if( best != nullptr && nid != 0 ) {
                    g_net_id_map[nid] = best;
                }
            }
        }

        if( best == nullptr ) {
            continue;
        }

        matched.insert( best );

        // Correct position if the server disagrees.
        if( best->pos_abs() != target && m.inbounds( target ) ) {
            const shared_ptr_fast<monster> occupant = ct.find( target );
            if( !occupant || occupant.get() == best ) {
                best->setpos( target, false );
            }
        }

        // Synthesise death messages from HP deltas before applying the new value.
        // "takes N damage" is intentionally omitted — attributed hit messages
        // from flush_action_msgs / host_capture_avatar_msgs cover those, and
        // the dumb HP-delta version creates confusing duplicates.
        if( nid != 0 && server_hp >= 0 ) {
            g_last_monster_hp[nid] = server_hp;
        }

        // Apply server HP. Kill locally if the server says it's dead.
        if( server_hp >= 0 ) {
            if( server_hp <= 0 ) {
                best->die( &m, nullptr );
            } else {
                best->set_hp( server_hp );
            }
        }

        // Sync facing direction for correct sprite flip.
        if( mo.has_int( "facing" ) ) {
            best->facing = mo.get_int( "facing" ) == 0
                           ? FacingDirection::LEFT : FacingDirection::RIGHT;
        }
    }

    // Any client monster in range that the server didn't mention is dead on the server.
    for( monster *mon : in_region ) {
        if( !matched.count( mon ) && !mon->is_dead() ) {
            mon->die( &m, nullptr );
        }
    }

    g->cleanup_dead();
}

static std::string build_viewport( const tripoint_bub_ms &center )
{
    constexpr int W = 41;
    constexpr int H = 21;

    map &m = get_map();
    avatar &u = get_avatar();
    npc *remote = g->critter_by_id<npc>( remote_player_npc_id );

    tripoint_bub_ms avatar_pos = u.pos_bub();

    // Map bounds in bubble coords
    const point_bub_ms map_min{ 0, 0 };
    const point_bub_ms map_max{ MAPSIZE_X - 1, MAPSIZE_Y - 1 };

    int ox = center.x() - W / 2;
    int oy = center.y() - H / 2;

    std::string tiles;
    tiles.reserve( W * H );

    for( int row = 0; row < H; ++row ) {
        for( int col = 0; col < W; ++col ) {
            int bx = ox + col;
            int by = oy + row;
            tripoint_bub_ms p{ bx, by, center.z() };

            if( bx < map_min.x() || bx > map_max.x() || by < map_min.y() || by > map_max.y() ) {
                tiles += ' ';
                continue;
            }

            // Player markers take priority
            if( remote && p == remote->pos_bub() ) {
                tiles += '2';
            } else if( p == avatar_pos ) {
                tiles += '@';
            } else {
                int sym = m.ter( p ).obj().symbol();
                char ch = ( sym >= 32 && sym < 127 ) ? static_cast<char>( sym ) : '?';
                if( ch == '\\' || ch == '"' ) {
                    tiles += '\\';
                }
                tiles += ch;
            }
        }
    }

    return "{\"w\":" + std::to_string( W ) +
           ",\"h\":" + std::to_string( H ) +
           ",\"ox\":" + std::to_string( ox ) +
           ",\"oy\":" + std::to_string( oy ) +
           ",\"tiles\":\"" + tiles + "\"}";
}

std::string serialize_remote_player_state()
{
    if( !remote_player_connected ) {
        return "{\"type\":\"state\",\"connected\":false}";
    }

    npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
    if( !remote ) {
        return "{\"type\":\"state\",\"connected\":false}";
    }

    tripoint_bub_ms pos_bub = remote->pos_bub();
    tripoint_abs_ms pos = remote->pos_abs();
    const avatar &host = get_avatar();
    tripoint_abs_ms host_pos = host.pos_abs();
    std::string viewport = build_viewport( pos_bub );
    std::string monsters     = build_monster_list( pos, 40 );

    // Scan for tile changes around both the remote player AND the host so that
    // doors/terrain the host interacts with also reach the client.
    std::string tile_changes = build_tile_changes( pos, 20 );
    if( host_pos != pos ) {
        std::string host_tc = build_tile_changes( host_pos, 20 );
        if( host_tc.size() > 2 ) { // not just "[]"
            if( tile_changes == "[]" ) {
                tile_changes = host_tc;
            } else {
                tile_changes = tile_changes.substr( 0, tile_changes.size() - 1 )
                               + "," + host_tc.substr( 1 );
            }
        }
    }

    // Per-bodypart HP for accurate client sidebar display.
    std::string bparts_json = "[";
    bool bfirst = true;
    for( const bodypart_id &bp : remote->get_all_body_parts() ) {
        if( !bfirst ) {
            bparts_json += ',';
        }
        bfirst = false;
        bparts_json += "{\"id\":\"" + bp.id().str() +
                       "\",\"hp\":" + std::to_string( remote->get_hp( bp ) ) +
                       ",\"hp_max\":" + std::to_string( remote->get_hp_max( bp ) ) + "}";
    }
    bparts_json += "]";

    const std::string host_male_str = host.male ? "true" : "false";

    // Build host appearance array (skin tone, eye color, hair style/color, facial hair).
    // Same format as "appearance" in worn_sync so the client can use one parser for both.
    static const std::vector<std::string> s_simple_app_types = {
        "skin_tone", "eye_color", "facial_hair", "hair_color"
    };
    std::string host_appearance_json = "[";
    bool happ_first = true;
    for( const std::string &mtype : s_simple_app_types ) {
        for( const trait_id &tid : get_mutations_in_type( mtype ) ) {
            if( host.has_trait( tid ) ) {
                if( !happ_first ) { host_appearance_json += ','; }
                happ_first = false;
                host_appearance_json += "{\"type\":\"" + mtype
                                      + "\",\"id\":\"" + tid.str() + "\"}";
                break;
            }
        }
    }
    for( const trait_and_var &tv : host.get_mutations_variants() ) {
        if( tv.trait.obj().types.count( "hair_style" ) ) {
            if( !happ_first ) { host_appearance_json += ','; }
            happ_first = false;
            host_appearance_json += "{\"type\":\"hair_style\",\"id\":\"" + tv.trait.str() + "\"";
            if( !tv.variant.empty() ) {
                host_appearance_json += ",\"var\":\"" + tv.variant + "\"";
            }
            host_appearance_json += '}';
            break;
        }
    }
    host_appearance_json += ']';

    // Host wielded weapon — sent so the client NPC proxy shows the correct item.
    std::string host_wielded_type;
    item_location host_wielded_loc = host.get_wielded_item();
    if( host_wielded_loc ) {
        host_wielded_type = host_wielded_loc->typeId().str();
    }

    // Host worn items — cached; JSON only rebuilt when worn list or appearance changes.
    static std::string g_host_worn_json_cache;
    static std::string g_host_worn_sig_cache;

    std::vector<const item *> host_worn_items;
    host.worn.inv_dump( host_worn_items );

    std::string host_worn_sig;
    for( const item *it : host_worn_items ) {
        host_worn_sig += it->typeId().str();
        if( it->has_itype_variant() ) {
            host_worn_sig += '[';
            host_worn_sig += it->itype_variant().id;
            host_worn_sig += ']';
        }
        host_worn_sig += ',';
    }
    host_worn_sig += '|' + host_appearance_json + '|' + host_wielded_type;

    std::string host_worn_json;
    if( host_worn_sig == g_host_worn_sig_cache ) {
        host_worn_json = g_host_worn_json_cache;
    } else {
        g_host_worn_sig_cache = host_worn_sig;
        host_worn_json = "[";
        bool hwfirst = true;
        for( const item *it : host_worn_items ) {
            if( !hwfirst ) {
                host_worn_json += ',';
            }
            hwfirst = false;
            host_worn_json += "{\"t\":\"" + it->typeId().str() + "\"";
            if( it->has_itype_variant() ) {
                host_worn_json += ",\"v\":\"" + it->itype_variant().id + "\"";
            }
            host_worn_json += "}";
        }
        host_worn_json += "]";
        g_host_worn_json_cache = host_worn_json;
        mp_log( "[cdda-mp] host_worn changed: [" + host_worn_sig + "]" );
    }

    // Build the messages JSON.  Two sources:
    //   1. g_action_msgs_pending — all messages from the last remote player action,
    //      already NPC→"You" substituted and filtered by flush_action_msgs().
    //   2. Name-filtered messages generated between actions (monster attacks on the
    //      NPC, etc.) that arrived since the last broadcast.
    const std::string &npc_name = remote->name;
    std::string msgs_json = "[";
    bool mfirst = true;

    // Helper: JSON-escape and append one message string.
    const auto append_msg = [&]( const std::string & text ) {
        std::string escaped;
        escaped.reserve( text.size() );
        for( char c : text ) {
            if( c == '\\' ) {
                escaped += "\\\\";
            } else if( c == '"' ) {
                escaped += "\\\"";
            } else {
                escaped += c;
            }
        }
        if( !mfirst ) {
            msgs_json += ',';
        }
        mfirst = false;
        msgs_json += '"' + escaped + '"';
    };

    // 1. Action-window messages (unfiltered except for host-avatar "You..." skips).
    for( const std::string &text : g_action_msgs_pending ) {
        append_msg( text );
    }
    g_action_msgs_pending.clear();

    // 1b. Host avatar combat messages forwarded with "You" → host name attribution.
    for( const std::string &text : g_host_action_msgs_pending ) {
        append_msg( text );
    }
    g_host_action_msgs_pending.clear();

    // 2. Between-action messages mentioning the remote player's NPC or their vehicle.
    // When the client is driving, physics messages (engine dies, collision, etc.)
    // name the vehicle not the player — include those so the driver sees them.
    std::string driving_veh_name;
    if( remote->controlling_vehicle && remote->in_vehicle ) {
        if( const optional_vpart_position vp_pos = get_map().veh_at( remote->pos_bub() ) ) {
            driving_veh_name = vp_pos->vehicle().name;
        }
    }
    const size_t current_msg_count = Messages::size();
    if( current_msg_count > g_last_forwarded_msg_count ) {
        const size_t new_count = current_msg_count - g_last_forwarded_msg_count;
        g_last_forwarded_msg_count = current_msg_count;
        const auto new_msgs = Messages::recent_messages( new_count );
        for( const auto &[time_str, text] : new_msgs ) {
            ( void )time_str;
            const bool has_npc    = !npc_name.empty() && text.find( npc_name ) != std::string::npos;
            const bool has_vehnam = !driving_veh_name.empty() &&
                                    text.find( driving_veh_name ) != std::string::npos;
            if( !has_npc && !has_vehnam ) {
                continue;  // skip messages unrelated to the remote player
            }
            std::string out = text;
            if( !npc_name.empty() ) {
                size_t p = 0;
                while( ( p = out.find( npc_name, p ) ) != std::string::npos ) {
                    out.replace( p, npc_name.size(), "You" );
                    p += 3;
                }
            }
            append_msg( out );
        }
    }

    msgs_json += ']';

    // Build vehicle sync payload: position, facing, and velocity for all vehicles
    // in the active reality bubble.  Clients use this to keep vehicle sprites in sync.
    std::string vehicles_json = "[";
    {
        bool vfirst = true;
        map &vmap = get_map();
        for( const wrapped_vehicle &wv : vmap.get_vehicles() ) {
            vehicle *v = wv.v;
            if( !v ) {
                continue;
            }
            // Assign a stable net ID on first encounter.
            auto vid_it = g_server_veh_ids.find( v );
            if( vid_it == g_server_veh_ids.end() ) {
                g_server_veh_ids[v] = ++g_next_net_id;
                vid_it = g_server_veh_ids.find( v );
            }
            const uint32_t nid = vid_it->second;
            const tripoint_abs_ms vabs = v->pos_abs();
            const int face_deg     = static_cast<int>( std::lround( to_degrees( v->face.dir() ) ) );
            const int turn_dir_deg = static_cast<int>( std::lround( to_degrees( v->turn_dir ) ) );
            // JSON-escape the vehicle name so special characters don't break the packet.
            std::string vname_escaped;
            vname_escaped.reserve( v->name.size() );
            for( char c : v->name ) {
                if( c == '\\' ) { vname_escaped += "\\\\"; }
                else if( c == '"' ) { vname_escaped += "\\\""; }
                else { vname_escaped += c; }
            }
            // Per-part state: open/enabled, HP, and fuel (for tanks/fuel stores).
            // Sequential index over get_all_parts() — host and client share the same
            // save so the iteration order is identical on both sides.
            std::string parts_json = "[";
            bool psfirst = true;
            int pidx = 0;
            for( const vpart_reference &vpr : v->get_all_parts() ) {
                const vehicle_part &vp = vpr.part();
                if( !psfirst ) {
                    parts_json += ',';
                }
                psfirst = false;
                parts_json += "{\"i\":" + std::to_string( pidx )
                              + ",\"open\":" + ( vp.open ? "true" : "false" )
                              + ",\"enabled\":" + ( vp.enabled ? "true" : "false" )
                              + ",\"hp\":" + std::to_string( vp.hp() );
                // Fuel stores: sync current fuel type and amount.
                if( vp.is_fuel_store( false ) && !vp.ammo_current().is_null() ) {
                    parts_json += ",\"fuel_type\":\"" + vp.ammo_current().str()
                                  + "\",\"fuel_amt\":" + std::to_string( vp.ammo_remaining() );
                }
                parts_json += "}";
                ++pidx;
            }
            parts_json += ']';

            if( !vfirst ) {
                vehicles_json += ',';
            }
            vfirst = false;
            vehicles_json += "{\"nid\":" + std::to_string( nid )
                             + ",\"x\":" + std::to_string( vabs.x() )
                             + ",\"y\":" + std::to_string( vabs.y() )
                             + ",\"z\":" + std::to_string( vabs.z() )
                             + ",\"face\":" + std::to_string( face_deg )
                             + ",\"turn_dir\":" + std::to_string( turn_dir_deg )
                             + ",\"vel\":" + std::to_string( v->velocity )
                             + ",\"cruise\":" + std::to_string( v->cruise_velocity )
                             + ",\"name\":\"" + vname_escaped + "\""
                             + ",\"parts\":" + parts_json + "}";
        }
    }
    vehicles_json += ']';

    return "{\"type\":\"state\","
           "\"calendar_turn\":" + std::to_string( to_turn<int>( calendar::turn ) ) + ","
           "\"host_name\":\"" + host.name + "\","
           "\"pos\":{\"x\":" + std::to_string( pos.x() ) +
           ",\"y\":" + std::to_string( pos.y() ) +
           ",\"z\":" + std::to_string( pos.z() ) + "},"
           "\"host_pos\":{\"x\":" + std::to_string( host_pos.x() ) +
           ",\"y\":" + std::to_string( host_pos.y() ) +
           ",\"z\":" + std::to_string( host_pos.z() ) + "},"
           "\"host_worn\":" + host_worn_json + ","
           "\"host_wielded\":\"" + host_wielded_type + "\","
           "\"host_male\":" + host_male_str + ","
           "\"host_appearance\":" + host_appearance_json + ","
           "\"host_move_mode\":\"" + host.move_mode.str() + "\","
           "\"host_facing\":" + std::to_string( host.facing == FacingDirection::LEFT ? 0 : 1 ) + ","
           "\"host_light\":" + std::to_string( host.active_light() ) + ","
           "\"bodyparts\":" + bparts_json +
           ",\"moves\":" + std::to_string( g_remote_moves ) +
           ",\"speed\":" + std::to_string( remote->get_speed() ) +
           ",\"client_move_mode\":\"" + remote->move_mode.str() + "\""
           ",\"client_stamina\":" + std::to_string( remote->get_stamina() ) +
           ",\"client_stamina_max\":" + std::to_string( remote->get_stamina_max() ) +
           ",\"client_ctrl_veh\":" + ( remote->controlling_vehicle ? "true" : "false" ) +
           [&]() -> std::string {
               if( !remote->controlling_vehicle ) { return ""; }
               map &vmap = get_map();
               const optional_vpart_position ovp = vmap.veh_at( remote->pos_bub() );
               if( !ovp ) { return ""; }
               const tripoint_abs_ms vabs = ovp->vehicle().pos_abs();
               return ",\"client_veh_pos\":{\"x\":" + std::to_string( vabs.x() )
                      + ",\"y\":" + std::to_string( vabs.y() )
                      + ",\"z\":" + std::to_string( vabs.z() ) + "}";
           }() +
           ",\"monsters\":" + monsters +
           ",\"tile_changes\":" + tile_changes +
           ",\"vehicles\":" + vehicles_json +
           ",\"msgs\":" + msgs_json +
           ",\"sfx\":" + [&]() -> std::string {
               std::string j = "[";
               bool first = true;
               for( const auto &e : g_host_sfx_queue ) {
                   if( !first ) { j += ','; }
                   first = false;
                   j += "{\"id\":\"" + json_escape_str( e.id )
                        + "\",\"v\":\"" + json_escape_str( e.variant )
                        + "\",\"vol\":" + std::to_string( e.vol ) + "}";
               }
               g_host_sfx_queue.clear();
               j += "]";
               return j;
           }() +
           ",\"map\":" + viewport + "}";
}

void client_wait_for_initial_position()
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while( !g_initial_teleport_done ) {
        if( std::chrono::steady_clock::now() >= deadline ) {
            std::cout << "[cdda-mp] Timed out waiting for initial position from server." << std::endl;
            break;
        }
        client_process_incoming();
        std::this_thread::sleep_for( 50ms );
    }
}

} // namespace cata_mp
