#include "mp_gamestate.h"
#include "mp_client_conn.h"
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
#include "type_id.h"
#include "ui_manager.h"
#ifdef TILES
#include "sdl_wrappers.h"
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

bool is_hosting()
{
    return get_active_server() != nullptr;
}

// The character_id of the remote player's NPC. Invalid when no remote player is connected.
static character_id remote_player_npc_id;
static bool remote_player_connected = false;
static std::string remote_player_name_;

// Client-side: NPC representing the host player in the client's local world.
static character_id client_host_npc_id;
static bool client_host_npc_spawned = false;

// Server: monotonically increasing counter for assigning monster network IDs.
static uint32_t g_next_net_id = 0;

// Server: cumulative AP for the remote player (replaces the NPC's own moves which
// are skipped by monmove since is_remote_player() returns true).
static int g_remote_moves = 0;

// Server: message log index at last state broadcast — used to forward only NEW messages.
static size_t g_last_forwarded_msg_count = 0;

// Server: messages captured during a remote player action that are forwarded
// verbatim (after NPC→"You" substitution) regardless of NPC name filter.
// Cleared by serialize_remote_player_state() when it consumes them.
static std::vector<std::string> g_action_msgs_pending;

// Client: maps server-assigned network IDs to local monster pointers.
// Rebuilt each sync tick from creature_tracker before applying updates.
static std::unordered_map<uint32_t, monster *> g_net_id_map;

// Client: action JSON queued to auto-fire once the server grants moves again.
// Latest keypress wins — pressing a different key replaces the queued action.
static std::string g_pending_action;

// Client: set when the host sends host_died or the socket drops.
// Suppresses further processing and the lost-connection spam.
static bool g_server_died = false;

// Client: set after sending an action, cleared when the server sends moves=0.
// While true, incoming state packets with moves>0 are stale pre-ack broadcasts
// and must be ignored — otherwise TCP-buffered grants re-unlock the client
// before the server has processed the action.
static bool g_client_waiting_for_ack = false;
// Timestamp of when the ack guard was set. Used to break deadlocks where the
// server never sends moves<=0 (e.g. after reconnect with a stale ack flag).
static std::chrono::steady_clock::time_point g_ack_set_time;

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
        const bool go = get_avatar().get_moves() > 0;
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
    std::string items_sig;  // "type:charges,..." — empty when no items
    std::string fields_sig; // "type:intensity,..." — empty when no fields
};
static std::unordered_map<tripoint_abs_ms, mp_tile_state> g_tile_baseline;

// Client→server tile baselines: track what was last sent so we only send diffs.
// Fields are always re-sent every turn because they decay server-side.
static std::unordered_map<tripoint_abs_ms, std::string> g_client_item_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_terfurn_baseline;

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
        // Skip host-avatar first-person messages that don't mention the remote NPC.
        const bool has_npc = !npc_name.empty() && text.find( npc_name ) != std::string::npos;
        if( !has_npc && text.size() >= 4 && text.substr( 0, 4 ) == "You " ) {
            continue;
        }
        std::string out = text;
        if( has_npc ) {
            size_t p = 0;
            while( ( p = out.find( npc_name, p ) ) != std::string::npos ) {
                out.replace( p, npc_name.size(), "You" );
                p += 3;
            }
        }
        g_action_msgs_pending.push_back( out );
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
                    std::cout << "[cdda-mp] Set wielded weapon: " << wielded_str << std::endl;
                }
            }
            // Apply the client's skin tone to the remote NPC.
            std::string skin_tone_str;
            jo.read( "skin_tone", skin_tone_str );
            if( !skin_tone_str.empty() ) {
                const trait_id new_tone( skin_tone_str );
                if( new_tone.is_valid() ) {
                    for( const trait_id &tid : get_mutations_in_type( "skin_tone" ) ) {
                        if( remote->has_trait( tid ) ) {
                            remote->unset_mutation( tid );
                        }
                    }
                    remote->set_mutation( new_tone );
                }
            }
            // Apply the client's hair style + color to the remote NPC.
            std::string hair_trait_str;
            std::string hair_variant_str;
            jo.read( "hair_trait", hair_trait_str );
            jo.read( "hair_variant", hair_variant_str );
            if( !hair_trait_str.empty() ) {
                const trait_id hair_tid( hair_trait_str );
                if( hair_tid.is_valid() ) {
                    for( const trait_id &tid : get_mutations_in_type( "hair_style" ) ) {
                        if( remote->has_trait( tid ) ) {
                            remote->unset_mutation( tid );
                        }
                    }
                    const mutation_variant *var = hair_variant_str.empty()
                                                  ? nullptr
                                                  : hair_tid.obj().variant( hair_variant_str );
                    mp_log( "[cdda-mp] worn_sync: hair=" + hair_trait_str
                            + "/" + hair_variant_str
                            + " valid=" + std::to_string( hair_tid.is_valid() )
                            + " var=" + ( var ? var->id : "NULL" ) );
                    remote->set_mutation( hair_tid, var );
                    mp_log( "[cdda-mp] worn_sync: has_trait after=" +
                            std::to_string( remote->has_trait( hair_tid ) ) );
                } else {
                    mp_log( "[cdda-mp] worn_sync: hair_tid INVALID: " + hair_trait_str );
                }
            } else {
                mp_log( "[cdda-mp] worn_sync: no hair_trait in message" );
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
                            JsonObject io = iv.get_object();
                            io.allow_omitted_members();
                            const itype_id tid( io.get_string( "t", "" ) );
                            if( tid.is_empty() || !tid.is_valid() ) {
                                continue;
                            }
                            item new_item( tid, calendar::turn );
                            if( io.has_string( "v" ) ) {
                                new_item.set_itype_variant( io.get_string( "v" ) );
                            }
                            if( io.has_int( "c" ) ) {
                                new_item.charges = io.get_int( "c" );
                            }
                            if( io.has_int( "bat" ) && !new_item.ammo_default().is_null() ) {
                                new_item.ammo_set( new_item.ammo_default(), io.get_int( "bat" ) );
                            } else if( new_item.type->light_emission > 0 && new_item.is_tool()
                                       && !new_item.ammo_default().is_null() ) {
                                new_item.ammo_set( new_item.ammo_default(), -1 );
                            }
                            m.add_item( bub, new_item );
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
                }
            }
        } catch( const JsonError & ) {}
    }

    // Wait — drain one turn's worth of AP.
    const bool is_wait = msg.find( "\"action\":\"wait\"" ) != std::string::npos ||
                         msg.find( "\"action\": \"wait\"" ) != std::string::npos;
    if( is_wait ) {
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
                    JsonObject io = iv.get_object();
                    io.allow_omitted_members();
                    const itype_id tid( io.get_string( "t", "" ) );
                    if( tid.is_valid() ) {
                        item dropped( tid );
                        const std::string var = io.get_string( "v", "" );
                        if( !var.empty() ) {
                            dropped.set_itype_variant( var );
                        }
                        here.add_item( bub_pos, std::move( dropped ) );
                        // Erase baseline so the next tile_changes scan always
                        // picks up this newly added item.
                        g_tile_baseline.erase( abs_pos );
                        mp_log( "[cdda-mp] drop: added " + tid.str()
                                + " at " + std::to_string( bub_pos.x() ) + ","
                                + std::to_string( bub_pos.y() ) );
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
                    m.close_door( bub, true, false );
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

    // Smash — parse absolute target and bash it using the NPC's smash ability.
    if( msg.find( "\"action\":\"smash\"" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            const tripoint_abs_ms abs_target{
                jo.get_int( "x" ), jo.get_int( "y" ), jo.get_int( "z" )
            };
            if( m.inbounds( abs_target ) ) {
                const tripoint_bub_ms bub = m.get_bub( abs_target );
                auto bash_map = remote->smash_ability();
                if( jo.has_int( "bash" ) ) {
                    const int client_bash = jo.get_int( "bash" );
                    const damage_type_id bash_type( "bash" );
                    bash_map[bash_type] = client_bash;
                }
                m.bash( bub, bash_map, false, true );
                mp_log( "[cdda-mp] smash @ " +
                        std::to_string( abs_target.x() ) + "," +
                        std::to_string( abs_target.y() ) );
            }
        } catch( const JsonError &e ) {
            std::cout << "[cdda-mp] smash parse error: " << e.what() << std::endl;
        }
        g_remote_moves -= remote->get_speed();
        g_client_acted_this_turn = true;
        flush_action_msgs( pre_action_msg, remote->name );
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
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
                std::cout << "[cdda-mp] position_sync: proxy NPC moved to z="
                          << abs_pos.z() << std::endl;
            }
        } catch( const JsonError &e ) {
            std::cout << "[cdda-mp] position_sync parse error: " << e.what() << std::endl;
        }
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

    tripoint_bub_ms cur = remote->pos_bub();
    tripoint_bub_ms next = cur;

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
            // Charge terrain-aware movement cost (accounts for NPC speed/encumbrance).
            const bool diag = ( std::abs( offset.x ) + std::abs( offset.y ) ) == 2;
            g_remote_moves -= remote->run_cost( m.move_cost( next ), diag );
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
    using namespace std::chrono_literals;
    const auto t_start = std::chrono::steady_clock::now();
    const auto deadline = t_start + 10s;
    while( !g_client_acted_this_turn && remote_player_connected ) {
        if( std::chrono::steady_clock::now() >= deadline ) {
            std::cout << "[cdda-mp] lockstep: timed out waiting for client action" << std::endl;
            break;
        }
        process_mp_events();
        ensure_mp_hud();
        // Let the host use UI actions (map, inventory, zoom, etc.) while waiting.
        // do_regular_action() blocks all world-mutating actions when hosting with moves<=0.
        g->mp_poll_input();
        std::this_thread::sleep_for( 16ms );
    }
    g_wait_elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start ).count() );
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

    mp_event event;
    while( get_mp_queue().pop( event ) ) {
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

        // Dress the host NPC with the items the host player is actually wearing,
        // and apply the host's skin tone. Signature-gated to avoid redoing every tick.
        if( jo.has_array( "host_worn" ) ) {
            // Build a fingerprint from the incoming worn list plus skin tone.
            std::string sig;
            for( const JsonValue &wv : jo.get_array( "host_worn" ) ) {
                JsonObject wo = wv.get_object();
                wo.allow_omitted_members();
                sig += wo.get_string( "t", "" ) + ',';
            }
            std::string incoming_skin_tone;
            jo.read( "host_skin_tone", incoming_skin_tone );
            std::string incoming_hair_sig;
            jo.read( "host_hair_trait", incoming_hair_sig );
            std::string incoming_wielded;
            jo.read( "host_wielded", incoming_wielded );
            sig += '|' + incoming_skin_tone + '|' + incoming_hair_sig + '|' + incoming_wielded;

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
                    // Apply skin tone.
                    if( !incoming_skin_tone.empty() ) {
                        const trait_id new_tone( incoming_skin_tone );
                        if( new_tone.is_valid() ) {
                            for( const trait_id &tid : get_mutations_in_type( "skin_tone" ) ) {
                                if( host_npc->has_trait( tid ) ) {
                                    host_npc->unset_mutation( tid );
                                }
                            }
                            host_npc->set_mutation( new_tone );
                        }
                    }
                    // Apply hair style + color.
                    std::string incoming_hair_trait;
                    std::string incoming_hair_variant;
                    jo.read( "host_hair_trait", incoming_hair_trait );
                    jo.read( "host_hair_variant", incoming_hair_variant );
                    if( !incoming_hair_trait.empty() ) {
                        const trait_id hair_tid( incoming_hair_trait );
                        if( hair_tid.is_valid() ) {
                            for( const trait_id &tid : get_mutations_in_type( "hair_style" ) ) {
                                if( host_npc->has_trait( tid ) ) {
                                    host_npc->unset_mutation( tid );
                                }
                            }
                            const mutation_variant *var = incoming_hair_variant.empty()
                                                          ? nullptr
                                                          : hair_tid.obj().variant( incoming_hair_variant );
                            host_npc->set_mutation( hair_tid, var );
                        }
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

        std::cout << "[cdda-mp] monster sync..." << std::flush;
        apply_monster_sync( jo );
        std::cout << " ok" << std::endl;

        std::cout << "[cdda-mp] tile sync..." << std::flush;
        apply_tile_changes( jo );
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
                g_client_waiting_for_ack = false;
                get_avatar().set_moves( srv_moves );
            } else if( !g_client_waiting_for_ack ) {
                // New grant and no unacked action in flight — apply normally.
                get_avatar().set_moves( srv_moves );
            }
            // else: srv_moves > 0 but waiting for ack → stale packet, skip.
        }

        // Display forwarded combat messages from the host (hits, misses, kills).
        if( jo.has_array( "msgs" ) ) {
            for( const JsonValue &mv : jo.get_array( "msgs" ) ) {
                add_msg( m_neutral, mv.get_string() );
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
    if( !g_pending_action.empty() && get_avatar().get_moves() > 0 ) {
        client_send( g_pending_action );
        g_pending_action.clear();
        get_avatar().set_moves( 0 );
        g_client_waiting_for_ack = true;
        g_ack_set_time = std::chrono::steady_clock::now();
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
            std::string items_sig;
            std::string items_json = "[]";
            auto items = m.i_at( bub );
            if( !items.empty() ) {
                items_json = "[";
                bool ifirst = true;
                for( const item &it : items ) {
                    const std::string var_str = it.has_itype_variant()
                                               ? it.itype_variant().id : "";
                    const int bat = it.ammo_remaining();
                    items_sig += it.typeId().str() + ':' + var_str + ':'
                                 + std::to_string( it.charges ) + ':'
                                 + std::to_string( bat ) + ',';
                    if( !ifirst ) {
                        items_json += ',';
                    }
                    ifirst = false;
                    items_json += "{\"t\":\"" + it.typeId().str() + "\"";
                    if( !var_str.empty() ) {
                        items_json += ",\"v\":\"" + var_str + "\"";
                    }
                    if( it.charges > 0 ) {
                        items_json += ",\"c\":" + std::to_string( it.charges );
                    }
                    if( bat > 0 ) {
                        items_json += ",\"bat\":" + std::to_string( bat );
                    }
                    items_json += "}";
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
                            std::to_string( abs.z() ) + " : " + items_sig );
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

            if( !terfurn_changed && !items_changed && fields_sig.empty() ) {
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
            out += "}";
        }
    }
    out += ']';
    return out;
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

    std::string enriched = json;
    if( !enriched.empty() && enriched.back() == '}' ) {
        enriched.pop_back();
        enriched += ",\"client_light\":" + std::to_string( cl );
        enriched += ",\"client_bleed\":" + bleed_json;
        enriched += ",\"client_tile_changes\":" + tile_changes;
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

    std::string skin_tone_str;
    for( const trait_id &tid : get_mutations_in_type( "skin_tone" ) ) {
        if( av.has_trait( tid ) ) {
            skin_tone_str = tid.str();
            break;
        }
    }
    std::string hair_trait_str;
    std::string hair_variant_str;
    for( const trait_and_var &tv : av.get_mutations_variants() ) {
        if( tv.trait.obj().types.count( "hair_style" ) ) {
            hair_trait_str = tv.trait.str();
            hair_variant_str = tv.variant;
            break;
        }
    }

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
                 + ",\"skin_tone\":\"" + skin_tone_str
                 + "\",\"hair_trait\":\"" + hair_trait_str
                 + "\",\"hair_variant\":\"" + hair_variant_str
                 + "\",\"wielded\":\"" + wielded_type + "\"}";
    client_send( worn_json );
}

void client_mark_action_sent()
{
    g_client_waiting_for_ack = true;
    g_ack_set_time = std::chrono::steady_clock::now();
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
            std::string items_sig;
            std::string items_json = "[]";
            auto items = m.i_at( bub );
            if( !items.empty() ) {
                items_json = "[";
                bool ifirst = true;
                for( const item &it : items ) {
                    const std::string var_str = it.has_itype_variant()
                                               ? it.itype_variant().id : "";
                    const int bat = it.ammo_remaining();
                    items_sig += it.typeId().str() + ':' + var_str + ':'
                                 + std::to_string( it.charges ) + ':'
                                 + std::to_string( bat ) + ',';
                    if( !ifirst ) {
                        items_json += ',';
                    }
                    ifirst = false;
                    items_json += "{\"t\":\"" + it.typeId().str() + "\"";
                    if( !var_str.empty() ) {
                        items_json += ",\"v\":\"" + var_str + "\"";
                    }
                    if( it.charges > 0 ) {
                        items_json += ",\"c\":" + std::to_string( it.charges );
                    }
                    if( bat > 0 ) {
                        items_json += ",\"bat\":" + std::to_string( bat );
                    }
                    items_json += "}";
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

            auto &baseline = g_tile_baseline[abs];
            if( baseline.ter == ter_str && baseline.furn == furn_str &&
                baseline.items_sig == items_sig && baseline.fields_sig == fields_sig ) {
                continue; // Nothing changed — skip this tile.
            }
            baseline.ter        = ter_str;
            baseline.furn       = furn_str;
            baseline.items_sig  = items_sig;
            baseline.fields_sig = fields_sig;

            if( !items_sig.empty() ) {
                mp_log( "tile_delta items @ " +
                        std::to_string( abs.x() ) + "," +
                        std::to_string( abs.y() ) + "," +
                        std::to_string( abs.z() ) + " : " + items_sig );
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
                   + ",\"fields\":" + fields_json + "}";
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
        out += "{\"nid\":" + std::to_string( mon_ptr->mp_net_id )
               + ",\"id\":\"" + mon_ptr->type->id.str() + "\""
               + ",\"x\":" + std::to_string( mp.x() )
               + ",\"y\":" + std::to_string( mp.y() )
               + ",\"z\":" + std::to_string( mp.z() )
               + ",\"hp\":" + std::to_string( mon_ptr->get_hp() ) + "}";
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
                JsonObject io = iv.get_object();
                io.allow_omitted_members();
                std::string type_str;
                io.read( "t", type_str );
                if( type_str.empty() ) {
                    continue;
                }
                applied += type_str + ' ';
                item new_item( itype_id( type_str ), calendar::turn );
                const std::string var_str = io.get_string( "v", "" );
                if( !var_str.empty() ) {
                    new_item.set_itype_variant( var_str );
                }
                if( io.has_int( "c" ) ) {
                    new_item.charges = io.get_int( "c" );
                }
                // Restore battery so light-emitting tools (phones, flashlights) work
                // correctly — getlight_emit() returns 0 with no battery (CHARGEDIM check).
                if( io.has_int( "bat" ) && !new_item.ammo_default().is_null() ) {
                    new_item.ammo_set( new_item.ammo_default(), io.get_int( "bat" ) );
                } else if( new_item.type->light_emission > 0 && new_item.is_tool()
                           && !new_item.ammo_default().is_null() ) {
                    new_item.ammo_set( new_item.ammo_default(), -1 );
                }
                m.add_item( bub, std::move( new_item ) );
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

        // Synthesise combat messages from HP deltas before applying the new value.
        if( nid != 0 && server_hp >= 0 ) {
            const auto prev_it = g_last_monster_hp.find( nid );
            if( prev_it != g_last_monster_hp.end() ) {
                const int prev_hp = prev_it->second;
                if( server_hp <= 0 && prev_hp > 0 ) {
                    add_msg( m_good, "The " + best->name() + " dies!" );
                } else if( server_hp < prev_hp ) {
                    add_msg( m_info, "The " + best->name() + " takes " +
                             std::to_string( prev_hp - server_hp ) + " damage." );
                }
            }
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

    // Host skin tone — transmitted so the client NPC proxy has the right appearance.
    std::string host_skin_tone;
    for( const trait_id &tid : get_mutations_in_type( "skin_tone" ) ) {
        if( host.has_trait( tid ) ) {
            host_skin_tone = tid.str();
            break;
        }
    }

    // Host hair style + color.
    std::string host_hair_trait;
    std::string host_hair_variant;
    for( const trait_and_var &tv : host.get_mutations_variants() ) {
        if( tv.trait.obj().types.count( "hair_style" ) ) {
            host_hair_trait = tv.trait.str();
            host_hair_variant = tv.variant;
            break;
        }
    }

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
    host_worn_sig += '|' + host_skin_tone + '|' + host_hair_trait + '|' + host_wielded_type;

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
        mp_log( "[cdda-mp] host_hair=" + host_hair_trait + "/" + host_hair_variant
                + " male=" + host_male_str );
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

    // 2. Between-action messages mentioning the remote player's NPC.
    const size_t current_msg_count = Messages::size();
    if( current_msg_count > g_last_forwarded_msg_count ) {
        const size_t new_count = current_msg_count - g_last_forwarded_msg_count;
        g_last_forwarded_msg_count = current_msg_count;
        const auto new_msgs = Messages::recent_messages( new_count );
        for( const auto &[time_str, text] : new_msgs ) {
            ( void )time_str;
            if( npc_name.empty() || text.find( npc_name ) == std::string::npos ) {
                continue;  // skip messages unrelated to the remote player
            }
            std::string out = text;
            size_t p = 0;
            while( ( p = out.find( npc_name, p ) ) != std::string::npos ) {
                out.replace( p, npc_name.size(), "You" );
                p += 3;
            }
            append_msg( out );
        }
    }

    msgs_json += ']';

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
           "\"host_skin_tone\":\"" + host_skin_tone + "\","
           "\"host_hair_trait\":\"" + host_hair_trait + "\","
           "\"host_hair_variant\":\"" + host_hair_variant + "\","
           "\"host_move_mode\":\"" + host.move_mode.str() + "\","
           "\"host_light\":" + std::to_string( host.active_light() ) + ","
           "\"bodyparts\":" + bparts_json +
           ",\"moves\":" + std::to_string( g_remote_moves ) +
           ",\"speed\":" + std::to_string( remote->get_speed() ) +
           ",\"monsters\":" + monsters +
           ",\"tile_changes\":" + tile_changes +
           ",\"msgs\":" + msgs_json +
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
