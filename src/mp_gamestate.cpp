#include "mp_gamestate.h"
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
#include "memory_fast.h"
#include "point.h"

#include <iostream>
#include <string>

namespace cata_mp {

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
        srv->broadcast( serialize_remote_player_state() + "\n" );
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

    map &m = get_map();
    tripoint_bub_ms cur = remote->pos_bub();
    tripoint_bub_ms next = cur;

    // Parse direction from JSON action
    if( msg.find( "\"dir\":\"n\"" ) != std::string::npos ) {
        next += tripoint( 0, -1, 0 );
    } else if( msg.find( "\"dir\":\"s\"" ) != std::string::npos ) {
        next += tripoint( 0, 1, 0 );
    } else if( msg.find( "\"dir\":\"e\"" ) != std::string::npos ) {
        next += tripoint( 1, 0, 0 );
    } else if( msg.find( "\"dir\":\"w\"" ) != std::string::npos ) {
        next += tripoint( -1, 0, 0 );
    } else if( msg.find( "\"dir\":\"ne\"" ) != std::string::npos ) {
        next += tripoint( 1, -1, 0 );
    } else if( msg.find( "\"dir\":\"nw\"" ) != std::string::npos ) {
        next += tripoint( -1, -1, 0 );
    } else if( msg.find( "\"dir\":\"se\"" ) != std::string::npos ) {
        next += tripoint( 1, 1, 0 );
    } else if( msg.find( "\"dir\":\"sw\"" ) != std::string::npos ) {
        next += tripoint( -1, 1, 0 );
    }

    // Only move if passable
    if( next != cur && !m.impassable( next ) ) {
        remote->setpos( m, next );
    }

    // Send updated state back
    server *srv = get_active_server();
    if( srv ) {
        srv->broadcast( serialize_remote_player_state() + "\n" );
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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

    return "{\"type\":\"state\","
           "\"pos\":{\"x\":" + std::to_string( pos.x() ) +
           ",\"y\":" + std::to_string( pos.y() ) +
           ",\"z\":" + std::to_string( pos.z() ) + "},"
           "\"hp\":" + std::to_string( hp ) +
           ",\"hp_max\":" + std::to_string( hp_max ) + "}";
}

} // namespace cata_mp
