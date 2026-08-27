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
intent_kind g_sent_kind = intent_kind::none;

// --- receiver state -------------------------------------------------------
point g_partner_dir = point( 0, 0 );
intent_kind g_partner_kind = intent_kind::none;
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

// Last "why nothing is drawn" reason we logged.  The draw path runs every
// frame, so the reason is only written when it CHANGES -- otherwise a hidden
// hint would spew a line per frame.
std::string g_last_skip_reason;
// Same idea for the success path: report a given intent once, not once a frame.
std::string g_last_draw_key;
// DIAG (2026-08-27): how many times the draw path has been entered since the
// last INTENT-RECV.  See the note at the INTENT-RECV log for what it settles.
int g_draw_calls = 0;

void log_skip( const std::string &reason )
{
    if( g_last_skip_reason == reason ) {
        return;
    }
    g_last_skip_reason = reason;
    mp_log( "[cdda-mp] INTENT-DRAW-SKIP: " + reason );
}

void send_intent( const std::string &payload )
{
    if( is_hosting() || is_server_mode() ) {
        server *s = get_active_server();
        mp_log( "[cdda-mp] INTENT-SEND: host broadcast srv=" +
                std::to_string( s != nullptr ) + " " + payload );
        if( s ) {
            s->post_broadcast( payload + "\n" );
        }
    } else if( is_client_mode() ) {
        mp_log( "[cdda-mp] INTENT-SEND: client_send " + payload );
        client_send( payload );
    } else {
        mp_log( "[cdda-mp] INTENT-SEND: DROPPED, neither host nor client " + payload );
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
    if( g_sent_kind == intent_kind::none ) {
        return;
    }
    g_sent_kind = intent_kind::none;
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
        // Not a step.  Whatever they are doing now -- including pausing, which
        // used to stage its own hint -- they are no longer thinking about the
        // thing we last advertised.
        mp_clear_intent();
        return;
    }

    avatar &av = get_avatar();

    // Every early return below logs its reason.  This only runs for movement and
    // pause keys, so it is low-frequency -- and without it, "nothing showed up"
    // is unattributable between "never staged", "never sent" and "never drawn".
    const std::string stage_ctx =
        std::string( " role=" ) + ( is_hosting() ? "host" : "client" ) +
        " act=" + std::to_string( act ) +
        " moves=" + std::to_string( av.get_moves() ) +
        " ack=" + std::to_string( is_client_waiting_for_ack() ) +
        " activity=" + ( av.activity ? av.activity.id().str() : "none" );

    // A direction key during an activity means "interrupt me", not "I intend to
    // step".  Without this the hint would lie through every long action.
    if( av.activity ) {
        mp_log( "[cdda-mp] INTENT-STAGE-SKIP: in_activity" + stage_ctx );
        mp_clear_intent();
        return;
    }

    // Only telegraph presses that are actually being thrown away.  With a grant
    // in hand the key executes normally and there is nothing to hint at -- and
    // this is also where a stale intent from the previous lock window gets
    // cleared, on the first key pressed after being granted.
    const bool locked = av.get_moves() <= 0 || is_client_waiting_for_ack();
    if( !locked ) {
        mp_log( "[cdda-mp] INTENT-STAGE-SKIP: not_locked" + stage_ctx );
        mp_clear_intent();
        return;
    }

    const tripoint_bub_ms target =
        av.pos_bub() + tripoint_rel_ms( it->second.x, it->second.y, 0 );
    if( step_is_wall_bump( target ) ) {
        mp_log( "[cdda-mp] INTENT-STAGE-SKIP: wall_bump" + stage_ctx );
        mp_clear_intent();
        return;
    }

    // Last press wins.  Same direction again (or a key repeat) is already on the
    // partner's screen, so it costs nothing.
    if( g_sent_kind == intent_kind::move && g_sent_dir == it->second ) {
        return;
    }

    g_sent_dir = it->second;
    g_sent_kind = intent_kind::move;
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
    // Unconditional, ahead of every guard: proves the packet crossed the wire
    // and reached the dispatcher at all.  Without this, a hint that never
    // appears is ambiguous between "never sent" and "arrived but rejected".
    mp_log( "[cdda-mp] INTENT-RECV-RAW: role=" +
            std::string( is_hosting() ? "host" : "client" ) + " " + msg );

    if( msg.find( "\"kind\":\"none\"" ) != std::string::npos ) {
        g_partner_kind = intent_kind::none;
        mp_log( "[cdda-mp] INTENT-RECV: clear" );
        return;
    }

    npc *partner = get_partner_npc();
    if( !partner ) {
        mp_log( "[cdda-mp] INTENT-RECV: REJECTED, no partner npc" );
        g_partner_kind = intent_kind::none;
        return;
    }

    // A "wait" packet from a build that still sends one falls through to the
    // dx/dy parse below, reads as (0,0) and clears -- which is the behavior we
    // want now that the pause telegraph is gone.
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
        g_partner_kind = intent_kind::none;
        return;
    }

    g_partner_dir = point( dx, dy );
    g_partner_anchor = get_map().get_abs( partner->pos_bub() );
    g_partner_staged_turn = calendar::turn;
    g_partner_kind = intent_kind::move;
    // DIAG (2026-08-27): draws_since_recv is the measurement that settles the
    // "arrows are delayed / do not update / only update when the host moves"
    // report.  The WAN logs already proved it is NOT the wire: 116 of 116
    // intents arrived, p50 99ms, max 377ms, zero drops.  So the question is
    // whether the draw path is being CALLED between receives, and neither
    // INTENT-DRAW-OK nor INTENT-DRAW-SKIP can answer that — both dedupe on
    // last-key/last-reason, so a stable arrow logs once no matter how many
    // frames run.  A count near 0 here means the host simply is not rendering
    // frames while it waits (fix the invalidation); a healthy count means the
    // draw runs and something inside it is rejecting the hint (fix the gate).
    mp_log( "[cdda-mp] INTENT-RECV: dx=" + std::to_string( dx ) +
            " dy=" + std::to_string( dy ) +
            " anchor=(" + std::to_string( g_partner_anchor.x() ) + "," +
            std::to_string( g_partner_anchor.y() ) + ")" +
            " draws_since_recv=" + std::to_string( g_draw_calls ) );
    g_draw_calls = 0;
}

intent_kind mp_partner_intent( int view_z, tripoint_bub_ms &hint_pos, point &dir )
{
    // DIAG (2026-08-27): counts every entry into the draw path, including the
    // early-out below.  Read and reset at each INTENT-RECV — see the note there.
    ++g_draw_calls;
    if( g_partner_kind == intent_kind::none ) {
        return intent_kind::none;
    }
    npc *partner = get_partner_npc();
    if( !partner ) {
        log_skip( "no partner npc" );
        g_partner_kind = intent_kind::none;
        return intent_kind::none;
    }
    // Clear-on-move: the partner moving IS the intent resolving.  Done locally
    // so the hint dies on the same frame their position updates, whichever of
    // the two messages happens to land first.
    if( get_map().get_abs( partner->pos_bub() ) != g_partner_anchor ) {
        g_partner_kind = intent_kind::none;
        mp_log( "[cdda-mp] INTENT-EXPIRE: partner moved" );
        return intent_kind::none;
    }
    if( calendar::turn - g_partner_staged_turn > intent_max_age_turns * 1_turns ) {
        g_partner_kind = intent_kind::none;
        mp_log( "[cdda-mp] INTENT-EXPIRE: aged out" );
        return intent_kind::none;
    }

    const tripoint_bub_ms ppos = partner->pos_bub();
    const tripoint_bub_ms target =
        ppos + tripoint_rel_ms( g_partner_dir.x, g_partner_dir.y, 0 );

    // A hint on another z-level is dropped here rather than silently in the
    // renderer -- that check used to live in cata_tiles.cpp with no logging,
    // which made it a blind spot exactly like the one being chased.
    if( target.z() != view_z ) {
        log_skip( "z mismatch: target_z=" + std::to_string( target.z() ) +
                  " view_z=" + std::to_string( view_z ) );
        return intent_kind::none;
    }

    // Never hint at a tile we cannot see: a marker behind a wall would leak both
    // the partner's position and the fact that a walkable tile exists there.
    if( !get_avatar().sees( get_map(), target ) ) {
        log_skip( "target not visible at (" + std::to_string( target.x() ) + "," +
                  std::to_string( target.y() ) + "," + std::to_string( target.z() ) + ")" );
        return intent_kind::none;
    }

    g_last_skip_reason.clear();
    hint_pos = target;
    dir = g_partner_dir;

    // Fires once per distinct intent (the draw path runs every frame, so it is
    // keyed on kind+dir+target).  This is the line that separates "the arrow was
    // never drawn" from "the arrow was drawn and you could not see it" -- the
    // one distinction the earlier logging could not make.
    const std::string draw_key =
        std::to_string( static_cast<int>( g_partner_kind ) ) + ":" +
        std::to_string( g_partner_dir.x ) + "," + std::to_string( g_partner_dir.y ) + ":" +
        std::to_string( target.x() ) + "," + std::to_string( target.y() );
    if( draw_key != g_last_draw_key ) {
        g_last_draw_key = draw_key;
        const tripoint_bub_ms me = get_avatar().pos_bub();
        mp_log( "[cdda-mp] INTENT-DRAW-OK: kind=move"
                " dir=(" + std::to_string( g_partner_dir.x ) + "," +
                std::to_string( g_partner_dir.y ) + ")" +
                " target_bub=(" + std::to_string( target.x() ) + "," +
                std::to_string( target.y() ) + "," + std::to_string( target.z() ) + ")" +
                " partner_bub=(" + std::to_string( ppos.x() ) + "," +
                std::to_string( ppos.y() ) + "," + std::to_string( ppos.z() ) + ")" +
                " me_bub=(" + std::to_string( me.x() ) + "," +
                std::to_string( me.y() ) + "," + std::to_string( me.z() ) + ")" );
    }
    return g_partner_kind;
}

} // namespace cata_mp
