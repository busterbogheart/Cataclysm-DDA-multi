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

// Server: set when the remote player has submitted at least one real action
// this turn.  Cleared by grant_client_turn(); checked by wait_for_client_action().
static bool g_client_acted_this_turn = false;

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
    return "?";
}

struct mp_hud_t {
    catacurses::window win;
    ui_adaptor ui;

    static constexpr int W = 46;
    static constexpr int H = 6;

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

        const bool client = is_client_mode();
        const std::string title = client ? " MP Client " : " MP Server ";
        mvwprintz( win, point( ( W - static_cast<int>( title.size() ) ) / 2, 0 ),
                   c_cyan, title );

        const int turn = to_turn<int>( calendar::turn );

        if( client ) {
            const avatar &av = get_avatar();
            const int moves = av.get_moves();
            const int speed = av.get_speed();

            mvwprintz( win, point( 2, 1 ), c_white, "Turn: %-8d  Speed: %3d", turn, speed );

            const nc_color mc = moves > 0 ? c_green : ( moves < 0 ? c_red : c_yellow );
            mvwprintz( win, point( 2, 2 ), c_white, "Moves: " );
            mvwprintz( win, point( 9, 2 ), mc, "%+-6d", moves );
            mvwprintz( win, point( 16, 2 ), moves > 0 ? c_green : c_red,
                       moves > 0 ? "ready " : "locked" );

            const std::string pend = pending_label();
            mvwprintz( win, point( 2, 3 ), c_white, "Queued: " );
            mvwprintz( win, point( 10, 3 ),
                       pend == "\xe2\x80\x94" ? c_dark_gray : c_yellow, pend );

            // Row 4: who has the turn right now
            if( moves > 0 ) {
                mvwprintz( win, point( 2, 4 ), c_green,  "You: acting  " );
                mvwprintz( win, point( 15, 4 ), c_yellow, "Host: waiting" );
            } else {
                mvwprintz( win, point( 2, 4 ), c_yellow, "You: waiting " );
                mvwprintz( win, point( 15, 4 ), c_green,  "Host: acting " );
            }
        } else {
            const avatar &host = get_avatar();
            const int host_moves = host.get_moves();
            const server *srv = get_active_server();
            const uint16_t port = srv ? srv->port() : 0;

            mvwprintz( win, point( 2, 1 ), c_white, "Turn: %-8d  Port: %-5u", turn, port );

            // Row 2: host moves
            const nc_color hmc = host_moves > 0 ? c_green : ( host_moves < 0 ? c_red : c_yellow );
            mvwprintz( win, point( 2, 2 ), c_white, "Host   mv: " );
            mvwprintz( win, point( 13, 2 ), hmc, "%+-6d", host_moves );
            mvwprintz( win, point( 20, 2 ), c_white, "spd: %3d", host.get_speed() );

            if( remote_player_connected ) {
                npc *remote = g->critter_by_id<npc>( remote_player_npc_id );
                // Row 3: client AP + waiting indicator
                const nc_color cmc = g_remote_moves > 0 ? c_green :
                                     ( g_remote_moves < 0 ? c_red : c_yellow );
                mvwprintz( win, point( 2, 3 ), c_white, "Client AP: " );
                mvwprintz( win, point( 13, 3 ), cmc, "%+-6d", g_remote_moves );
                if( remote ) {
                    mvwprintz( win, point( 20, 3 ), c_white, "spd: %3d", remote->get_speed() );
                }
                const bool need_client = !g_client_acted_this_turn;
                mvwprintz( win, point( 30, 3 ),
                           need_client ? c_yellow : c_green,
                           need_client ? "WAIT" : "ok  " );

                // Row 4: remote player name + position
                if( remote ) {
                    const tripoint_abs_ms p = remote->pos_abs();
                    mvwprintz( win, point( 2, 4 ), c_white,
                               "%-12s @ %d, %d, %d",
                               remote_player_name_, p.x(), p.y(), p.z() );
                }
            } else {
                mvwprintz( win, point( 2, 3 ), c_dark_gray, "Waiting for remote player..." );
            }
        }

        wnoutrefresh( win );
    }
};

static std::unique_ptr<mp_hud_t> g_mp_hud;

void ensure_mp_hud()
{
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

    // Always respawn near the host player regardless of saved position
    remote->spawn_at_precise( m.get_abs( spawn_pos ) );
    overmap_buffer.insert_npc( remote );
    g->load_npcs();

    remote_player_npc_id = remote->getID();

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
    g_last_forwarded_msg_count = Messages::size();  // don't forward pre-connect history
    mp_save_npc_ids();  // persist ID so next session can clean it up

    add_msg( m_good, "%s has connected and joined the game.", name );
    std::cout << "[cdda-mp] Spawned remote player '" << name << "' at "
              << spawn_pos.x() << "," << spawn_pos.y() << std::endl;

    // Send initial state to player 2
    server *srv = get_active_server();
    if( srv ) {
        srv->post_broadcast( serialize_remote_player_state() + "\n" );
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

    // Worn-item sync — client sends this once after joining so the remote NPC
    // wears the same gear the client player actually has equipped.
    if( msg.find( "\"action\":\"worn_sync\"" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_array( "worn" ) ) {
                remote->clear_worn();
                for( const JsonValue &wv : jo.get_array( "worn" ) ) {
                    JsonObject wo = wv.get_object();
                    wo.allow_omitted_members();
                    const itype_id tid( wo.get_string( "t", "" ) );
                    if( tid.is_valid() ) {
                        remote->worn.wear_item( *remote, item( tid ),
                                               false, false, true, true );
                    }
                }
                std::vector<item *> applied_worn;
                remote->worn.inv_dump( applied_worn );
                std::string worn_list;
                for( const item *wi : applied_worn ) {
                    worn_list += wi->typeId().str() + ' ';
                }
                mp_log( "[cdda-mp] worn_sync applied: [" + worn_list + "]" );
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
                    std::cout << "[cdda-mp] Applied skin tone '" << skin_tone_str
                              << "' to remote player." << std::endl;
                }
            }
        } catch( const JsonError &e ) {
            mp_log( std::string( "[cdda-mp] worn_sync parse error: " ) + e.what() );
        }
        return;
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

    // Pickup — client runs the normal single-player dialog locally; this message
    // just drains AP on the server so the move budget stays in sync.
    const bool is_pickup = msg.find( "\"action\":\"pickup\"" ) != std::string::npos ||
                           msg.find( "\"action\": \"pickup\"" ) != std::string::npos;
    if( is_pickup ) {
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
                m.bash( bub, remote->smash_ability() );
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
        } else if( !m.impassable( next ) ) {
            remote->setpos( m, next );
            // Charge terrain-aware movement cost (accounts for NPC speed/encumbrance).
            const bool diag = ( std::abs( offset.x ) + std::abs( offset.y ) ) == 2;
            g_remote_moves -= remote->run_cost( m.move_cost( next ), diag );
        } else {
            // Bump-to-open: try to open a door on the target tile (follows CDDA rules —
            // respects locks, handles etc).  If it's not openable, fail silently.
            if( m.open_door( *remote, next, true, false ) ) {
                g_remote_moves -= remote->get_speed();
            }
        }
    }

    // Capture all messages generated during this action (hits, misses, damage,
    // kills) for forwarding to the client regardless of NPC name filter.
    flush_action_msgs( pre_action_msg, remote->name );

    g_client_acted_this_turn = true;
    g_remote_moves = 0;  // lock client after one action; grant_client_turn() restores on next turn

    // Send updated state back
    server *srv = get_active_server();
    if( srv ) {
        srv->post_broadcast( serialize_remote_player_state() + "\n" );
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
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while( !g_client_acted_this_turn && remote_player_connected ) {
        if( std::chrono::steady_clock::now() >= deadline ) {
            std::cout << "[cdda-mp] lockstep: timed out waiting for client action" << std::endl;
            break;
        }
        process_mp_events();
        ensure_mp_hud();
        ui_manager::redraw();
        refresh_display();
#ifdef TILES
        // Pump the macOS/SDL event queue so the host window stays responsive
        // (no spinning beach ball) while waiting for the client to act.
        SDL_PumpEvents();
#endif
        std::this_thread::sleep_for( 16ms );
    }
#ifdef TILES
    // Drain any keyboard/mouse events that accumulated while the gate was active
    // so they don't replay as unintended actions on the next input poll.
    SDL_Event ev;
    while( SDL_PollEvent( &ev ) ) {}
#endif
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
            sig += '|' + incoming_skin_tone;

            if( sig != g_client_host_worn_sig && client_host_npc_spawned ) {
                std::cout << "[cdda-mp] dressing host NPC..." << std::flush;
                g_client_host_worn_sig = sig;
                npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
                if( host_npc ) {
                    host_npc->clear_worn();
                    for( const JsonValue &wv : jo.get_array( "host_worn" ) ) {
                        JsonObject wo = wv.get_object();
                        wo.allow_omitted_members();
                        const itype_id tid( wo.get_string( "t", "" ) );
                        if( tid.is_valid() ) {
                            host_npc->worn.wear_item( *host_npc, item( tid ),
                                                      false, false, true, true );
                        }
                    }
                    // Apply skin tone to the host NPC proxy.
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
                }
                std::cout << " ok" << std::endl;
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

        // Apply server-authoritative move budget.
        // While g_client_waiting_for_ack is set (we sent an action but haven't
        // received the server's moves=0 ack yet), ignore moves>0 packets —
        // those are stale pre-ack broadcasts still in the TCP buffer.
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
        // Just sent the join — immediately follow with our worn-item list and skin
        // tone so the server can dress the remote NPC in our actual gear/appearance.
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
        std::string worn_json = "{\"action\":\"worn_sync\",\"worn\":[";
        bool wfirst = true;
        for( const item *it : worn_items ) {
            if( !wfirst ) {
                worn_json += ',';
            }
            wfirst = false;
            worn_json += "{\"t\":\"" + it->typeId().str() + "\"}";
        }
        // Include wielded weapon so the host NPC proxy attacks with the right item.
        std::string wielded_type;
        item_location wielded = av.get_wielded_item();
        if( wielded ) {
            wielded_type = wielded->typeId().str();
        }
        worn_json += "],\"skin_tone\":\"" + skin_tone_str
                     + "\",\"wielded\":\"" + wielded_type + "\"}";
        std::string worn_log;
        for( const item *it : worn_items ) {
            worn_log += it->typeId().str() + ' ';
        }
        mp_log( "[cdda-mp] worn_sync sent: [" + worn_log + "]" );
        client_send( worn_json );
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
    }
}

void client_queue_action( const std::string &json )
{
    g_pending_action = json;
}

void client_mark_action_sent()
{
    g_client_waiting_for_ack = true;
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
                    items_sig += it.typeId().str() + ':' +
                                 std::to_string( it.charges ) + ',';
                    if( !ifirst ) {
                        items_json += ',';
                    }
                    ifirst = false;
                    items_json += "{\"t\":\"" + it.typeId().str() + "\"";
                    if( it.charges > 0 ) {
                        items_json += ",\"c\":" + std::to_string( it.charges );
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
                mp_log( "[cdda-mp] tile_delta items @ " +
                        std::to_string( abs.x() ) + "," +
                        std::to_string( abs.y() ) + "," +
                        std::to_string( abs.z() ) + " : " + items_sig );
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
                if( io.has_int( "c" ) ) {
                    new_item.charges = io.get_int( "c" );
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

    // Host worn items — so the client can dress the host NPC correctly.
    std::vector<const item *> host_worn_items;
    host.worn.inv_dump( host_worn_items );
    std::string host_worn_json = "[";
    bool hwfirst = true;
    for( const item *it : host_worn_items ) {
        if( !hwfirst ) {
            host_worn_json += ',';
        }
        hwfirst = false;
        host_worn_json += "{\"t\":\"" + it->typeId().str() + "\"}";
    }
    host_worn_json += "]";

    // Host skin tone — transmitted so the client NPC proxy has the right appearance.
    std::string host_skin_tone;
    for( const trait_id &tid : get_mutations_in_type( "skin_tone" ) ) {
        if( host.has_trait( tid ) ) {
            host_skin_tone = tid.str();
            break;
        }
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
           "\"host_name\":\"" + host.name + "\","
           "\"pos\":{\"x\":" + std::to_string( pos.x() ) +
           ",\"y\":" + std::to_string( pos.y() ) +
           ",\"z\":" + std::to_string( pos.z() ) + "},"
           "\"host_pos\":{\"x\":" + std::to_string( host_pos.x() ) +
           ",\"y\":" + std::to_string( host_pos.y() ) +
           ",\"z\":" + std::to_string( host_pos.z() ) + "},"
           "\"host_worn\":" + host_worn_json + ","
           "\"host_skin_tone\":\"" + host_skin_tone + "\","
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
