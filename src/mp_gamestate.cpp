#include "mp_gamestate.h"
#include "mp_client_conn.h"
#include "mp_queue.h"
#include "mp_server.h"

#include "avatar.h"
#include "calendar.h"
#include "cata_utility.h"
#include "character_id.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "game.h"
#include "item.h"
#include "json.h"
#include "json_loader.h"
#include "monster.h"
#include "mtype.h"
#include "map.h"
#include "map_scale_constants.h"
#include "memory_fast.h"
#include "messages.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "path_info.h"
#include "point.h"
#include "teleport.h"
#include "type_id.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cata_mp {

static bool server_mode_ = false;

bool is_server_mode()
{
    return server_mode_;
}

void set_server_mode( bool enabled )
{
    server_mode_ = enabled;
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

// Client: maps server-assigned network IDs to local monster pointers.
// Rebuilt each sync tick from creature_tracker before applying updates.
static std::unordered_map<uint32_t, monster *> g_net_id_map;

// Client: last confirmed position of the remote player (our avatar as seen by the server).
// Used as the center of the monster sync region.
static tripoint_abs_ms g_mp_remote_pos{ 0, 0, 0 };

// Server: per-tile baseline (ter + furn + item fingerprint) for dirty-tile tracking.
struct mp_tile_state {
    std::string ter;
    std::string furn;
    std::string items_sig; // "type:charges,type:charges,..." — empty when no items
};
static std::unordered_map<tripoint_abs_ms, mp_tile_state> g_tile_baseline;

// Client: last known HP per net ID — used to synthesise combat hit/death messages.
static std::unordered_map<uint32_t, int> g_last_monster_hp;

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

    avatar &u = get_avatar();
    map &m = get_map();

    // Find nearest passable tile within 4 tiles, searching outward from north
    tripoint_bub_ms spawn_pos = u.pos_bub();
    bool found = false;
    for( int radius = 1; radius <= 4 && !found; ++radius ) {
        for( int dy = -radius; dy <= radius && !found; ++dy ) {
            for( int dx = -radius; dx <= radius && !found; ++dx ) {
                if( std::abs( dx ) != radius && std::abs( dy ) != radius ) {
                    continue; // only check the ring edge
                }
                tripoint_bub_ms candidate = u.pos_bub() + tripoint( dx, dy, 0 );
                if( !m.impassable( candidate ) ) {
                    spawn_pos = candidate;
                    found = true;
                }
            }
        }
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
    remote_player_connected = true;

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

    remote_player_connected = false;
    remote_player_npc_id = character_id();
    add_msg( m_bad, "The other player has disconnected." );
    std::cout << "[cdda-mp] Remote player removed from world." << std::endl;
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

    // Wait — broadcast state without moving
    const bool is_wait = msg.find( "\"action\":\"wait\"" ) != std::string::npos ||
                         msg.find( "\"action\": \"wait\"" ) != std::string::npos;
    if( is_wait ) {
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( serialize_remote_player_state() + "\n" );
        }
        return;
    }

    map &m = get_map();

    // Pickup — clear items from the NPC's current tile (inventory sync is future work).
    const bool is_pickup = msg.find( "\"action\":\"pickup\"" ) != std::string::npos ||
                           msg.find( "\"action\": \"pickup\"" ) != std::string::npos;
    if( is_pickup ) {
        m.i_clear( remote->pos_bub() );
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
    if( dir_match( "n" ) ) {
        next += tripoint( 0, -1, 0 );
    } else if( dir_match( "s" ) ) {
        next += tripoint( 0, 1, 0 );
    } else if( dir_match( "e" ) ) {
        next += tripoint( 1, 0, 0 );
    } else if( dir_match( "w" ) ) {
        next += tripoint( -1, 0, 0 );
    } else if( dir_match( "ne" ) ) {
        next += tripoint( 1, -1, 0 );
    } else if( dir_match( "nw" ) ) {
        next += tripoint( -1, -1, 0 );
    } else if( dir_match( "se" ) ) {
        next += tripoint( 1, 1, 0 );
    } else if( dir_match( "sw" ) ) {
        next += tripoint( -1, 1, 0 );
    }

    // Move or attack, matching single-player bump-to-attack behaviour.
    // Check for a creature first — melee_attack applies regardless of tile passability.
    if( next != cur ) {
        const tripoint_abs_ms next_abs = m.get_abs( next );
        const shared_ptr_fast<monster> target = get_creature_tracker().find( next_abs );
        if( target ) {
            remote->melee_attack( *target, true );
        } else if( !m.impassable( next ) ) {
            remote->setpos( m, next );
        }
    }

    // Send updated state back
    server *srv = get_active_server();
    if( srv ) {
        srv->post_broadcast( serialize_remote_player_state() + "\n" );
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool is_remote_player( character_id id )
{
    return remote_player_connected && id == remote_player_npc_id;
}

void process_mp_events()
{
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

    if( !m.inbounds( abs_pos ) ) {
        g->place_player_overmap( project_to<coords::omt>( abs_pos ), false );
    }

    const tripoint_bub_ms new_pos = m.get_bub( abs_pos );
    if( new_pos != u.pos_bub() ) {
        u.setpos( m, new_pos );
        g->update_map( u );
    }
}

static void update_client_host_npc( const tripoint_abs_ms &abs_pos, const std::string &name )
{
    map &m = get_map();

    if( !client_host_npc_spawned ) {
        shared_ptr_fast<npc> host_npc = make_shared_fast<npc>();
        host_npc->normalize();
        host_npc->name = name.empty() ? "host" : name;
        host_npc->spawn_at_precise( abs_pos );
        overmap_buffer.insert_npc( host_npc );
        g->load_npcs();
        client_host_npc_id = host_npc->getID();
        client_host_npc_spawned = true;
        std::cout << "[cdda-mp] Spawned host NPC '" << host_npc->name << "' at abs "
                  << abs_pos.x() << "," << abs_pos.y() << std::endl;
        return;
    }

    npc *host_npc = g->critter_by_id<npc>( client_host_npc_id );
    if( !host_npc ) {
        client_host_npc_spawned = false;
        return;
    }
    if( !name.empty() && host_npc->name != name ) {
        host_npc->name = name;
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
    const bool is_state = msg.find( "\"type\":\"state\"" ) != std::string::npos ||
                          msg.find( "\"type\": \"state\"" ) != std::string::npos;
    if( !is_state ) {
        std::cout << "[cdda-mp] " << msg << std::endl;
        return false;
    }
    if( msg.find( "\"connected\":false" ) != std::string::npos ) {
        add_msg( m_bad, "Lost connection to server." );
        return true;
    }

    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();

        if( jo.has_object( "pos" ) ) {
            JsonObject pos = jo.get_object( "pos" );
            pos.allow_omitted_members();
            g_mp_remote_pos = tripoint_abs_ms{
                pos.get_int( "x" ), pos.get_int( "y" ), pos.get_int( "z" )
            };
            client_teleport_avatar( g_mp_remote_pos );
        }

        std::string host_name;
        jo.read( "host_name", host_name );

        if( jo.has_object( "host_pos" ) ) {
            JsonObject hpos = jo.get_object( "host_pos" );
            hpos.allow_omitted_members();
            const tripoint_abs_ms host_pos{
                hpos.get_int( "x" ), hpos.get_int( "y" ), hpos.get_int( "z" )
            };
            update_client_host_npc( host_pos, host_name );
        }

        apply_monster_sync( jo );
        apply_tile_changes( jo );

    } catch( const JsonError &e ) {
        std::cout << "[cdda-mp] JSON parse error in state: " << e.what() << std::endl;
    }
    return true;
}

void client_process_incoming()
{
    // Send the join message on the first tick — the save is loaded by now.
    client_send_join();
    std::string msg;
    while( client_recv_pop( msg ) ) {
        apply_one_state_message( msg );
    }
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

            auto &baseline = g_tile_baseline[abs];
            if( baseline.ter == ter_str && baseline.furn == furn_str &&
                baseline.items_sig == items_sig ) {
                continue; // Nothing changed — skip this tile.
            }
            baseline.ter       = ter_str;
            baseline.furn      = furn_str;
            baseline.items_sig = items_sig;

            if( !first ) {
                out += ',';
            }
            first = false;
            out += "{\"x\":" + std::to_string( abs.x() )
                   + ",\"y\":" + std::to_string( abs.y() )
                   + ",\"z\":" + std::to_string( abs.z() )
                   + ",\"ter\":\"" + ter_str + "\""
                   + ",\"furn\":\"" + furn_str + "\""
                   + ",\"items\":" + items_json + "}";
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
            m.i_clear( bub );
            for( const JsonValue &iv : to.get_array( "items" ) ) {
                JsonObject io = iv.get_object();
                io.allow_omitted_members();
                std::string type_str;
                io.read( "t", type_str );
                if( type_str.empty() ) {
                    continue;
                }
                item new_item( itype_id( type_str ), calendar::turn );
                if( io.has_int( "c" ) ) {
                    new_item.charges = io.get_int( "c" );
                }
                m.add_item( bub, std::move( new_item ) );
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
    int hp = remote->get_hp();
    int hp_max = remote->get_hp_max();
    std::string viewport = build_viewport( pos_bub );
    std::string monsters     = build_monster_list( pos, 40 );
    std::string tile_changes = build_tile_changes( pos, 20 );

    return "{\"type\":\"state\","
           "\"host_name\":\"" + host.name + "\","
           "\"pos\":{\"x\":" + std::to_string( pos.x() ) +
           ",\"y\":" + std::to_string( pos.y() ) +
           ",\"z\":" + std::to_string( pos.z() ) + "},"
           "\"host_pos\":{\"x\":" + std::to_string( host_pos.x() ) +
           ",\"y\":" + std::to_string( host_pos.y() ) +
           ",\"z\":" + std::to_string( host_pos.z() ) + "},"
           "\"hp\":" + std::to_string( hp ) +
           ",\"hp_max\":" + std::to_string( hp_max ) +
           ",\"monsters\":" + monsters +
           ",\"tile_changes\":" + tile_changes +
           ",\"map\":" + viewport + "}";
}

} // namespace cata_mp
