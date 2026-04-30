#include "mp_gamestate.h"
#include "mp_client_conn.h"
#include "mp_queue.h"
#include "mp_server.h"

#include "avatar.h"
#include "character_id.h"
#include "creature_tracker.h"
#include "game.h"
#include "map.h"
#include "messages.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "type_id.h"
#include "map_scale_constants.h"
#include "memory_fast.h"
#include "point.h"

#include <iostream>
#include <string>

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
    remote->normalize();
    remote->name = name;
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

    // Only move if passable
    if( next != cur && !m.impassable( next ) ) {
        remote->setpos( m, next );
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
}

static int parse_int_in( const std::string &json, const std::string &key )
{
    for( const std::string &sep : { std::string( "\":" ), std::string( "\": " ) } ) {
        const std::string needle = "\"" + key + sep;
        auto pos = json.find( needle );
        if( pos != std::string::npos ) {
            pos += needle.size();
            while( pos < json.size() && json[pos] == ' ' ) {
                ++pos;
            }
            if( pos < json.size() && ( std::isdigit( static_cast<unsigned char>( json[pos] ) ) ||
                                       json[pos] == '-' ) ) {
                return std::stoi( json.substr( pos ) );
            }
        }
    }
    return 0;
}

void client_process_incoming()
{
    std::string msg;
    while( client_recv_pop( msg ) ) {
        // Ignore non-state messages (hello, welcome, player_joined, etc.)
        const bool is_state = msg.find( "\"type\":\"state\"" ) != std::string::npos ||
                              msg.find( "\"type\": \"state\"" ) != std::string::npos;
        if( !is_state ) {
            std::cout << "[cdda-mp server] " << msg << std::endl;
            continue;
        }
        if( msg.find( "\"connected\":false" ) != std::string::npos ) {
            add_msg( m_bad, "Lost connection to server." );
            continue;
        }

        // Parse pos block only
        const auto pos_start = msg.find( "\"pos\":" );
        if( pos_start == std::string::npos ) {
            continue;
        }
        const auto obj_open  = msg.find( '{', pos_start );
        const auto obj_close = msg.find( '}', obj_open );
        if( obj_open == std::string::npos || obj_close == std::string::npos ) {
            continue;
        }
        const std::string pos_obj = msg.substr( obj_open, obj_close - obj_open + 1 );

        const int x = parse_int_in( pos_obj, "x" );
        const int y = parse_int_in( pos_obj, "y" );
        const int z = parse_int_in( pos_obj, "z" );

        const int bx = std::max( 0, std::min( x, MAPSIZE_X - 1 ) );
        const int by = std::max( 0, std::min( y, MAPSIZE_Y - 1 ) );

        avatar &u = get_avatar();
        map &m = get_map();
        const tripoint_bub_ms new_pos{ bx, by, z };
        if( new_pos != u.pos_bub() ) {
            u.setpos( m, new_pos );
        }
    }
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

    tripoint_bub_ms pos = remote->pos_bub();
    int hp = remote->get_hp();
    int hp_max = remote->get_hp_max();
    std::string viewport = build_viewport( pos );

    return "{\"type\":\"state\","
           "\"pos\":{\"x\":" + std::to_string( pos.x() ) +
           ",\"y\":" + std::to_string( pos.y() ) +
           ",\"z\":" + std::to_string( pos.z() ) + "},"
           "\"hp\":" + std::to_string( hp ) +
           ",\"hp_max\":" + std::to_string( hp_max ) +
           ",\"map\":" + viewport + "}";
}

} // namespace cata_mp
