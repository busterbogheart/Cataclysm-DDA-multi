#include "mp_intent.h"

#include <exception>
#include <map>
#include <string>

#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "json_loader.h"
#include "map.h"
#include "mp_client_conn.h"
#include "mp_gamestate.h"
#include "mp_server.h"
#include "npc.h"
#include "player_activity.h"

namespace cata_mp
{

namespace
{

// The eight walking directions.  Everything else (fire, examine, menus) is out
// of scope for v1 -- see the ranked list in ROADMAP under "Other indications".
const std::map<action_id, point> &intent_dirs()
{
    static const std::map<action_id, point> dirs = {
        { ACTION_MOVE_FORTH,       point( 0, -1 ) },
        { ACTION_MOVE_BACK,        point( 0,  1 ) },
        { ACTION_MOVE_RIGHT,       point( 1,  0 ) },
        { ACTION_MOVE_LEFT,        point( -1,  0 ) },
        { ACTION_MOVE_FORTH_RIGHT, point( 1, -1 ) },
        { ACTION_MOVE_FORTH_LEFT,  point( -1, -1 ) },
        { ACTION_MOVE_BACK_RIGHT,  point( 1,  1 ) },
        { ACTION_MOVE_BACK_LEFT,   point( -1,  1 ) },
    };
    return dirs;
}

// --- sender state ---------------------------------------------------------
// The direction the partner is currently drawing for us.  Kept so a packet only
// goes out when the staged direction actually changes: holding a movement key
// repeats the same action and must not become one packet per key repeat.
point g_sent_dir = point( 0, 0 );
bool g_sent_any = false;

// --- receiver state -------------------------------------------------------
point g_partner_dir = point( 0, 0 );
bool g_partner_live = false;
// Where the partner stood when the intent arrived.  If they have moved since,
// the intent resolved and the hint is stale.  We drop it locally rather than
// waiting for the clear packet, because the position update and the clear are
// separate messages with no ordering guarantee between them -- position-first
// would draw the old direction against the new position for a frame or two,
// i.e. the hint appears to jump a tile ahead before snapping away.
tripoint_abs_ms g_partner_anchor;
// Deliberately measured in TURNS, not wall-clock.  During a long deliberation
// the turn counter is not advancing (the game is blocked on the thinker), so a
// turn-based age-out correctly persists the hint for the whole pause.  A
// seconds-based timeout would expire it precisely when it is wanted.
time_point g_partner_staged_turn;
constexpr int intent_max_age_turns = 2;

void send_intent( const std::string &payload )
{
    if( is_hosting() || is_server_mode() ) {
        if( server *s = get_active_server() ) {
            s->post_broadcast( payload + "\n" );
        }
    } else if( is_client_mode() ) {
        client_send( payload );
    }
}

// Would this step be a plain wall bump?  The client's movement path already
// short-circuits those before it reaches us, but the host's lock gate does not,
// and a hint pointing into solid rock reads as a bug rather than as intent.
bool step_is_wall_bump( const tripoint_bub_ms &target )
{
    map &here = get_map();
    if( !here.inbounds( target ) || !here.impassable( target ) ) {
        return false;
    }
    // A creature standing in a doorway or an openable door is still a
    // meaningful thing to be heading toward.
    if( get_creature_tracker().find( here.get_abs( target ) ) ) {
        return false;
    }
    return !here.open_door( get_avatar(), target, true, true );
}

} // namespace

void mp_clear_intent()
{
    if( !g_sent_any ) {
        return;
    }
    g_sent_any = false;
    g_sent_dir = point( 0, 0 );
    mp_log( "[cdda-mp] INTENT-CLEAR: sending none" );
    send_intent( "{\"type\":\"intent\",\"kind\":\"none\"}" );
}

void mp_stage_intent_action( action_id act )
{
    if( !is_hosting() && !is_client_mode() ) {
        return;
    }

    const auto it = intent_dirs().find( act );
    if( it == intent_dirs().end() ) {
        // Not a movement key.  Whatever they are doing now, they are no longer
        // thinking about the direction we last advertised.
        mp_clear_intent();
        return;
    }

    avatar &av = get_avatar();

    // A direction key during an activity means "interrupt me", not "I intend to
    // step".  Without this the hint would lie through every long action.
    if( av.activity ) {
        mp_clear_intent();
        return;
    }

    // Only telegraph presses that are actually being thrown away.  With a grant
    // in hand the key executes normally and there is nothing to hint at -- and
    // this is also where a stale intent from the previous lock window gets
    // cleared, on the first key pressed after being granted.
    const bool locked = av.get_moves() <= 0 || is_client_waiting_for_ack();
    if( !locked ) {
        mp_clear_intent();
        return;
    }

    const tripoint_bub_ms target =
        av.pos_bub() + tripoint_rel_ms( it->second.x, it->second.y, 0 );
    if( step_is_wall_bump( target ) ) {
        mp_clear_intent();
        return;
    }

    // Last press wins.  Same direction again (or a key repeat) is already on the
    // partner's screen, so it costs nothing.
    if( g_sent_any && g_sent_dir == it->second ) {
        return;
    }

    g_sent_dir = it->second;
    g_sent_any = true;
    mp_log( "[cdda-mp] INTENT-STAGE: dx=" + std::to_string( it->second.x ) +
            " dy=" + std::to_string( it->second.y ) +
            " moves=" + std::to_string( av.get_moves() ) +
            " ack=" + std::to_string( is_client_waiting_for_ack() ) );
    send_intent( "{\"type\":\"intent\",\"kind\":\"move\",\"dx\":" +
                 std::to_string( it->second.x ) + ",\"dy\":" +
                 std::to_string( it->second.y ) + "}" );
}

void mp_handle_intent_recv( const std::string &msg )
{
    if( msg.find( "\"kind\":\"none\"" ) != std::string::npos ) {
        g_partner_live = false;
        mp_log( "[cdda-mp] INTENT-RECV: clear" );
        return;
    }

    npc *partner = get_partner_npc();
    if( !partner ) {
        g_partner_live = false;
        return;
    }

    int dx = 0;
    int dy = 0;
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        dx = jo.get_int( "dx", 0 );
        dy = jo.get_int( "dy", 0 );
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] INTENT-RECV: parse failed: " ) + e.what() );
        return;
    }
    if( dx == 0 && dy == 0 ) {
        g_partner_live = false;
        return;
    }

    g_partner_dir = point( dx, dy );
    g_partner_anchor = get_map().get_abs( partner->pos_bub() );
    g_partner_staged_turn = calendar::turn;
    g_partner_live = true;
    mp_log( "[cdda-mp] INTENT-RECV: dx=" + std::to_string( dx ) +
            " dy=" + std::to_string( dy ) +
            " anchor=(" + std::to_string( g_partner_anchor.x() ) + "," +
            std::to_string( g_partner_anchor.y() ) + ")" );
}

bool mp_partner_intent_offset( point &out )
{
    if( !g_partner_live ) {
        return false;
    }
    npc *partner = get_partner_npc();
    if( !partner ) {
        g_partner_live = false;
        return false;
    }
    // Clear-on-move: the partner moving IS the intent resolving.  Done locally
    // so the hint dies on the same frame their position updates, whichever of
    // the two messages happens to land first.
    if( get_map().get_abs( partner->pos_bub() ) != g_partner_anchor ) {
        g_partner_live = false;
        mp_log( "[cdda-mp] INTENT-EXPIRE: partner moved" );
        return false;
    }
    if( calendar::turn - g_partner_staged_turn > intent_max_age_turns * 1_turns ) {
        g_partner_live = false;
        mp_log( "[cdda-mp] INTENT-EXPIRE: aged out" );
        return false;
    }
    out = g_partner_dir;
    return true;
}

} // namespace cata_mp
