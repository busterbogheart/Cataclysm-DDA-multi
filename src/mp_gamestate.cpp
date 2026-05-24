#include "mp_gamestate.h"
#include "mp_client_conn.h"
#include "do_turn.h"
#include "input.h"
#include "mp_queue.h"
#include "mp_server.h"

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "character_martial_arts.h"
#include "martialarts.h"
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
#include "panels.h"
#include "ui_manager.h"
#include "rng.h"
#include "units.h"
#include "units_utility.h"
#include "skill.h"
#include "popup.h"
#include "string_input_popup.h"
#include "uilist.h"
#include "veh_type.h"
#include "vehicle.h"
#include "worldfactory.h"
#ifdef TILES
#include "sdl_wrappers.h"
#include "sounds.h"
#endif
#include <chrono>
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
    // Wall-clock ms since the previous mp_log line — lets us read the log as a
    // timeline ("this step took 47ms") without having to add timing helpers
    // around every call site.  Reset each line: prefix shows the gap since the
    // last log call, so a long gap = something blocked the main thread.
    static std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const long long delta_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>( now - last ).count();
    last = now;
    // stdout only — start-mp.sh tees stdout to /tmp/cdda-mp-{server,client}.log.
    // Writing to the file from here too would double every log line.
    std::cout << "[+" << delta_ms << "ms] " << msg << std::endl;
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

// Server: nids for which the full vehicle save-format snapshot has already been
// broadcast to the current client.  First broadcast of a previously-unknown
// vehicle includes the "snapshot" payload so the client can mirror the SP
// map::add_vehicle path; subsequent broadcasts stay slim.  Cleared on client
// connect so a fresh client receives snapshots for every visible vehicle.
// Also dropped per-nid when the vehicle's parts vector changes size (install,
// remove, fold/unfold, damage-purge) so a structural mutation triggers a full
// re-snapshot — the slim parts/cargo deltas cover state-on-existing-parts only.
static std::unordered_set<uint32_t> g_client_known_veh_nids;

// Server: per-nid baseline of the parts vector size from the last broadcast.
// Any change (install/remove/purge) invalidates that nid's snapshot tracking
// so the client receives a fresh full snapshot rather than indices that drift
// from the host's vector.
static std::unordered_map<uint32_t, size_t> g_server_veh_parts_count;

// Server: nids broadcast in the most recent state packet.  Diffed against the
// current iteration to detect vehicles that have disappeared (folded, fully
// destroyed, driven out of bubble) so we can emit a "removed_vehicles" entry
// in the state packet for the client to clean up.
static std::unordered_set<uint32_t> g_server_veh_live_nids;

// Client: maps server vehicle network IDs to the last-known absolute tile position.
// Used to look up the vehicle object before moving it to the server-authoritative position.
static std::unordered_map<uint32_t, tripoint_abs_ms> g_client_veh_pos;

// Server: cumulative AP for the remote player (replaces the NPC's own moves which
// are skipped by monmove since is_remote_player() returns true).
static int g_remote_moves = 0;

// Server: true while wait_for_client_action() is blocking.  Used by the HUD
// strip so it shows red (can't act) even when host moves > 0.
static bool g_host_waiting_for_client = false;
// Host-only one-shot: when set, the next serialize_remote_player_state()
// broadcast carries "wake_client":true and the flag is cleared.  Drives the
// host→client direction of the tap-on-shoulder action.
static bool g_pending_wake_client = false;

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

// Server: monotonically increasing sequence number, incremented each grant_client_turn().
// Included in every state packet so the client can distinguish a fresh grant from a
// TCP-buffered duplicate of a previous grant.
static uint32_t g_grant_seq = 0;

// Client: last grant_seq that was successfully applied (moves > 0 path).
// Guards the "new grant" branch: a packet with grant_seq <= g_client_last_grant_seq
// is a stale buffered duplicate and its moves value is ignored.
static uint32_t g_client_last_grant_seq = 0;

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

// Host: the host's own last input action.  Captured at the end of handle_action
// so the local "Queued" HUD field actually reflects the host's most recent key,
// not the client's (which was the previous, misleading behavior).
static std::string g_last_host_action_label = "\xe2\x80\x94";

// Partner activity id (e.g. "ACT_DROP", or "" for none).  On the host this is
// the client's activity (set from action packets); on the client this is the
// host's activity (set from state packets).  Display-only — the actual
// activity ticks on the side that owns the player.  Used by the Co-op HUD and
// transition-edge messages.
static std::string g_partner_activity;
// Progress % (0-100) of the partner's current activity.  Forwarded each
// action/state packet alongside g_partner_activity.  Read by the Co-op panel.
static int g_partner_activity_pct = 0;
// Total moves required by the partner's current activity (act.moves_total).
// Read by the bump-menu predicate to decide whether the "Help with task"
// option should appear (gate: >= HELPER_MIN_MOVES_TOTAL).  Zero when idle.
static int g_partner_activity_moves_total = 0;
// Last calendar turn the partner reported.  Used to display sync drift in the
// Co-op panel: drift = local calendar - partner calendar.  Under lockstep
// both sides should always advance together; nonzero drift is a useful sanity
// indicator for the player.
static int g_partner_calendar_turn = 0;
// Last name the partner reported.  Used by the Co-op panel as a fallback when
// the local proxy NPC isn't (yet) resolvable — proxy spawn races the panel on
// first connect; this lets the panel still show *something* instead of
// "Partner unknown".
static std::string g_partner_name_cached;

// Host-only: tracks whether the proxy NPC was previously resolvable.  Used to
// detect the alive→gone transition so we can distinguish "proxy died in
// combat on host" (notify client, disconnect) from "proxy was never spawned
// yet" or "still null after a recent disconnect".  Cleared on disconnect; set
// to true on the first grant where the proxy resolves.
static bool g_proxy_was_alive = false;
static std::string g_partner_activity_prev;

// Client's avatar activity id snapshot at the start of each do_turn iteration.
// Sent over the wire instead of av.activity at enrich-time because the activity
// can finish (set_to_null) mid-turn, leaving av.activity null when the wait
// dispatch runs — which would mask the activity from the host's HUD.
static std::string g_client_turn_activity;

// Strip "ACT_" prefix and lowercase the rest so the HUD shows "drop" not
// "ACT_DROP".  Returns "—" for empty.
static std::string mp_format_activity( const std::string &act_id )
{
    if( act_id.empty() ) {
        return "\xe2\x80\x94";
    }
    std::string s = act_id;
    if( s.rfind( "ACT_", 0 ) == 0 ) {
        s = s.substr( 4 );
    }
    for( char &c : s ) {
        c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
    }
    return s;
}

// Look up the partner's display name.  On host: the proxy NPC representing
// the connected client.  On client: the proxy NPC representing the host.
// Falls back to "Partner" when the proxy isn't yet known.  This is the single
// source of truth for any MP message that needs to address the other player.
static std::string mp_partner_display_name()
{
    const character_id &id = is_hosting() ? remote_player_npc_id : client_host_npc_id;
    if( id.is_valid() ) {
        if( npc *n = g->critter_by_id<npc>( id ) ) {
            if( !n->name.empty() ) {
                return n->name;
            }
        }
    }
    return _( "Partner" );
}

// Translate an activity id (e.g. ACT_VEHICLE) to a human verb phrase suitable
// for "<name> begins <X>." sentences.  Prefers the activity_type::verb()
// translation maintained in JSON ("constructing a vehicle", "reading", etc.).
// Falls back to a stripped/lowercased id when no verb is registered.
static std::string mp_activity_verb_phrase( const std::string &act_id )
{
    if( act_id.empty() ) {
        return std::string();
    }
    const activity_id aid( act_id );
    if( aid.is_valid() ) {
        const std::string v = aid->verb().translated();
        if( !v.empty() ) {
            return v;
        }
    }
    return mp_format_activity( act_id );
}

// Forward decl — defined further down with the other co-op helper functions.
static void mp_cancel_help_if_partner_done();

// Templates wire-sync helpers (definitions below alongside notify_session_ending).
// Forward-declared so the host- and client-side message dispatchers can call
// them; both dispatchers live above the definitions.
static void mp_handle_templates_list( const std::string &msg );
static void mp_handle_template_request( const std::string &msg );
static void mp_handle_template_data( const std::string &msg );
static void mp_send_payload( const std::string &payload );

// Fire "<partner> begins <verb>." / "<partner> has finished." on the
// transition edges.  Caller is responsible for setting g_partner_activity to
// the latest value before calling this.
static void mp_partner_activity_transition_check()
{
    if( g_partner_activity == g_partner_activity_prev ) {
        return;
    }
    const std::string partner_name = mp_partner_display_name();
    if( g_partner_activity_prev.empty() && !g_partner_activity.empty() ) {
        add_msg( m_info, _( "%1$s begins %2$s." ), partner_name,
                 mp_activity_verb_phrase( g_partner_activity ) );
    } else if( !g_partner_activity_prev.empty() && g_partner_activity.empty() ) {
        // Use the just-ended activity's verb so the message reads "<name> has
        // finished reading." instead of the generic "<name> has finished."
        add_msg( m_info, _( "%1$s has finished %2$s." ), partner_name,
                 mp_activity_verb_phrase( g_partner_activity_prev ) );
    }
    g_partner_activity_prev = g_partner_activity;
    // Partner's activity changed — if we were helping with the OLD one and
    // they've moved on (or stopped entirely), drop our local commitment so
    // the SP helper bonus stops being applied.
    mp_cancel_help_if_partner_done();
}

// Client → host message forwarding.  When the client's avatar generates a
// notable "You ..." message (e.g. "Now reading X", "You start crafting Y"),
// we capture it here and tack it onto the next outgoing action so the host
// sees a name-substituted version ("Roy now reads X") in their own log.
static size_t g_client_msg_watermark = 0;
static std::vector<std::string> g_client_msgs_pending;

// Separation warning tier: 0=ok, 1=warn (≥50 tiles), 2=danger (≥57 tiles).
// Shared by both host and client; resets on connect/disconnect.
// Hysteresis: step up at 50/57, step down at 44/50.
static int g_separation_tier = 0;
static void check_separation_warning( const tripoint_abs_ms &a, const tripoint_abs_ms &b );

// Client: luminance emitted by the host player (flashlight, mutations, etc.).
// Received from state packet each turn and injected into the lighting pass.
static float g_mp_host_luminance = 0.0f;

// Host: luminance emitted by the remote player (client). Received from each
// action packet and injected into the host's lighting pass at the proxy NPC position.
static float g_mp_remote_player_luminance = 0.0f;

static const efftype_id effect_bleed( "bleed" );

// ---------------------------------------------------------------------------
// Info panel (bottom-left corner)
// ---------------------------------------------------------------------------

struct mp_hud_t {
    catacurses::window win;
    ui_adaptor ui;

    static constexpr int W = 56;
    static constexpr int H = 3;

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

        // Partner-centric single content row.  Host sees the connected client;
        // client sees the host.  Both read from the local NPC proxy that the
        // wire-state apply path keeps fresh (HP, move_mode, position), plus
        // the latest g_partner_activity / g_partner_activity_pct received
        // from the wire.
        const character_id &partner_id = is_hosting()
                                         ? remote_player_npc_id
                                         : client_host_npc_id;
        npc *partner = partner_id.is_valid()
                       ? g->critter_by_id<npc>( partner_id )
                       : nullptr;
        // Fallback: id may be stale or invalid after a reconnect/respawn but
        // a matching NPC still exists in the active world.  Scan by cached
        // name so we can still pull HP / move_mode for the panel.
        if( !partner && !g_partner_name_cached.empty() ) {
            for( npc &candidate : g->all_npcs() ) {
                if( candidate.name == g_partner_name_cached ) {
                    partner = &candidate;
                    break;
                }
            }
        }

        if( !remote_player_connected && is_hosting() ) {
            mvwprintz( win, point( 2, 1 ), c_dark_gray, "%s",
                       _( "Partner not connected" ) );
            wnoutrefresh( win );
            return;
        }

        // Column 0: partner name.  Prefer the live proxy NPC's name; fall back
        // to the cached name from the wire when the proxy isn't resolvable
        // (first-frame race after connect, between npc despawn/respawn).
        // Truncated to 10 chars with a ".." continuation marker.
        std::string pname = partner ? partner->name : g_partner_name_cached;
        if( pname.empty() ) {
            pname = "Partner";
        }
        if( pname.size() > 10 ) {
            pname = pname.substr( 0, 8 ) + "..";
        }
        int x = 1;
        mvwprintz( win, point( x, 1 ), c_white, "%-10s", pname.c_str() );
        x += 11;

        // Move mode in square brackets — first char only (w/r/c/p) for compactness.
        // Requires the proxy NPC; show a placeholder when it isn't available.
        char mm = '?';
        if( partner ) {
            const std::string mode_str = partner->move_mode.str();
            if( !mode_str.empty() ) {
                mm = mode_str[0];
            }
        }
        mvwprintz( win, point( x, 1 ), partner ? c_white : c_dark_gray, "[%c]", mm );
        x += 4;

        // HP bar — 6 chars colored by the WORST body part's HP fraction.
        // Summing across all parts hid critical damage: a half-shredded
        // torso gets averaged out by full limbs and the bar stays in the
        // green band even when the partner is one hit from going down.
        // The min-across-parts metric answers "is my partner in trouble?"
        // which is what the player actually wants to glance at.
        const int bar_w = 6;
        int filled = 0;
        nc_color hp_color = c_dark_gray;
        if( partner ) {
            float worst = 1.0f;
            for( const bodypart_id &bp : partner->get_all_body_parts() ) {
                const int hpm = partner->get_hp_max( bp );
                if( hpm <= 0 ) {
                    continue;
                }
                const float f = std::clamp(
                                    static_cast<float>( partner->get_hp( bp ) ) / hpm,
                                    0.0f, 1.0f );
                if( f < worst ) {
                    worst = f;
                }
            }
            hp_color = worst > 0.66f ? c_green
                       : worst > 0.33f ? c_yellow : c_red;
            filled = static_cast<int>( std::round( worst * bar_w ) );
        }
        mvwprintz( win, point( x, 1 ), c_white, "[" );
        for( int i = 0; i < bar_w; ++i ) {
            mvwprintz( win, point( x + 1 + i, 1 ),
                       i < filled ? hp_color : c_dark_gray, "#" );
        }
        mvwprintz( win, point( x + 1 + bar_w, 1 ), c_white, "]" );
        x += bar_w + 3;

        // Activity + progress %.  Use the verb phrase (e.g. "reading",
        // "constructing a vehicle") so the panel matches the begin/finish
        // sentences.  Empty when partner is idle.
        if( !g_partner_activity.empty() ) {
            const std::string verb = mp_activity_verb_phrase( g_partner_activity );
            // Compose "<verb> NN%" — clamp verb length so the % stays on row.
            const int avail = W - x - 8; // reserve room for " NN%" + drift
            std::string vshown = verb;
            if( static_cast<int>( vshown.size() ) > avail ) {
                vshown = vshown.substr( 0, std::max( 0, avail - 2 ) ) + "..";
            }
            mvwprintz( win, point( x, 1 ), c_yellow, "%s", vshown.c_str() );
            x += static_cast<int>( vshown.size() ) + 1;
            mvwprintz( win, point( x, 1 ), c_light_blue, "%d%%",
                       g_partner_activity_pct );
            x += 5;
        }

        // Calendar drift indicator on the right edge.  Δ followed by signed
        // turn count.  Green when 0 (in sync), yellow nonzero, red large.
        // Always-on so it's a visible sanity light, not just an error popup.
        if( g_partner_calendar_turn != 0 ) {
            const int local_turn = to_turn<int>( calendar::turn );
            const int drift = local_turn - g_partner_calendar_turn;
            const nc_color dc = drift == 0 ? c_green
                                : std::abs( drift ) <= 1 ? c_yellow : c_red;
            const std::string ds = "Δ" + std::to_string( drift );
            mvwprintz( win, point( W - static_cast<int>( ds.size() ) - 1, 1 ),
                       dc, "%s", ds.c_str() );
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
    bool right_side;
    // Smooth the brief moves=0 gap between a sent action and the next grant —
    // without this, the strip blips red every grant→ack cycle even when the
    // player is fully unblocked.  Updated on each draw where go=true.
    mutable std::chrono::steady_clock::time_point last_go_time =
        std::chrono::steady_clock::now() - std::chrono::seconds( 10 );

    // Bar width in columns.  Two columns of full block chars gives a chunky
    // solid bar that's easy to read with peripheral vision while focused on
    // the game view.  One half-block column blended into the adjacent black
    // space and was hard to glance-read.
    static constexpr int strip_w = 2;

    explicit mp_strip_t( bool right = false ) : right_side( right ) {
        ui.on_screen_resize( [this]( ui_adaptor &ua ) {
            const int sidebar_w = panel_manager::get_manager().get_width_right();
            const int x = right_side
                          ? TERMX - sidebar_w - strip_w
                          : 0;
            mp_log( "[cdda-mp] mp_strip resize: right=" + std::to_string( right_side ) +
                    " TERMX=" + std::to_string( TERMX ) + " sidebar_w=" + std::to_string( sidebar_w ) +
                    " x=" + std::to_string( x ) );
            win = catacurses::newwin( TERMY, strip_w, point( x, 0 ) );
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
        // Smooth the lockstep ack-gap flicker.  Own wait activities bypass the
        // smoother — those are sticky states, not transient gaps.
        const auto now = std::chrono::steady_clock::now();
        if( go ) {
            last_go_time = now;
        }
        const auto since_go_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - last_go_time ).count();
        const bool show_green = !in_wait_act && ( go || since_go_ms < 400 );
        const nc_color c = show_green ? c_light_green : c_red;
        // █ full block — fills the whole cell vs the previous left/right half
        // block which left half the column black against the game view.
        static constexpr const char *ch = "\xe2\x96\x88";
        for( int y = 0; y < TERMY; y++ ) {
            for( int x = 0; x < strip_w; x++ ) {
                mvwprintz( win, point( x, y ), c, ch );
            }
        }
        wnoutrefresh( win );
    }
};

static std::unique_ptr<mp_strip_t> g_mp_strip;
static std::unique_ptr<mp_strip_t> g_mp_strip_right;
static std::unique_ptr<mp_hud_t> g_mp_hud;

void ensure_mp_hud()
{
    // Strips rendered first so panel draws on top in the overlap zone.
    if( !g_mp_strip ) {
        g_mp_strip = std::make_unique<mp_strip_t>( false );
    }
    g_mp_strip->ui.invalidate_ui();
    if( !g_mp_strip_right ) {
        g_mp_strip_right = std::make_unique<mp_strip_t>( true );
    }
    g_mp_strip_right->ui.invalidate_ui();
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
static mp_tile_state compute_tile_state( const tripoint_abs_ms &abs );

// Client→server tile baselines: track what was last sent so we only send diffs.
// Fields are host-authoritative — the host generates the dust/blood/etc. that
// the client sees via tile_changes, and the client must not echo them back or
// the host's own state ping-pongs back as ~80 KB per turn.  apply_tile_changes
// refreshes these baselines from server-pushed state so the next outgoing
// build_client_tile_changes recognizes the tile as unchanged and skips it.
static std::unordered_map<tripoint_abs_ms, std::string> g_client_item_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_terfurn_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_trap_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_graffiti_baseline;
static std::unordered_map<tripoint_abs_ms, std::string> g_client_field_baseline;
// Client→server vehicle cargo baseline.  Keyed by the absolute tile position
// of the cargo vpart so the host can find the vehicle + part by tile lookup.
// Mirrors the item baseline but for items stored inside vehicle cargo parts
// (trunks, freezers, lockers, etc.).
static std::unordered_map<tripoint_abs_ms, std::string> g_client_veh_cargo_baseline;
// Server→client vehicle cargo baseline.  Same keying as the client direction —
// without this the client can't see items the host drops into trunks/seats/etc.,
// and its stale snapshot would then overwrite the host on the next client drop.
static std::unordered_map<tripoint_abs_ms, std::string> g_host_veh_cargo_baseline;
// Client→server worn-list baseline.  When the worn signature changes (e.g.
// drop_activity_actor peeled a worn garment off as part of a drop), we trigger
// a client_resync_worn() so the host's proxy mirrors the new worn list.
// Plain signature string keyed on typeId + variant + wielded id.
static std::string g_client_worn_baseline;

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
    // Cache the partner's name so the Co-op panel has a stable fallback even
    // if the NPC pointer briefly becomes unresolvable (load/unload races).
    g_partner_name_cached = name;

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
    g_client_known_veh_nids.clear();  // re-snapshot every visible vehicle for the fresh client
    g_server_veh_parts_count.clear();
    g_server_veh_live_nids.clear();
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

    // Templates wire-sync: send host's local template list so the client can
    // request any it doesn't already have.  Cheap one-shot exchange — the
    // client sends its own list independently on its side.
    mp_templates_sync_on_join();
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
    g_proxy_was_alive = false;
    g_separation_tier = 0;
    // Do NOT reset g_grant_seq here.  It must stay monotonically increasing so
    // that a reconnecting client (which resets g_client_last_grant_seq=0 via the
    // join path) always sees seq > 0 and accepts the first new grant.  Resetting
    // to 0 here caused a deadlock: client last_seq=N, server restarts at seq=1..N
    // which were all skipped as "old seq".
    mp_save_npc_ids();  // ID is now invalid — clears the cleanup file entry
    add_msg( m_bad, "The other player has disconnected." );
    std::cout << "[cdda-mp] Remote player removed from world." << std::endl;
}

// After substituting an NPC name → "You", the verb is still third-person singular.
// Strip the suffix so "You guts" → "You gut", "You misses" → "You miss", etc.
// Also fixes "You's " → "your " for possessive constructions.
// Rewrite a first-person message ("You drop your hand mirror on the grass.")
// into a third-person version with the subject's name and proper conjugation
// ("Wilford Rubin drops Wilford Rubin's hand mirror on the grass.").
//
// Handles three substitutions and one conjugation pass:
//   1. Leading "You " → "<name> " + s/-verb conjugation
//   2. Mid-string " you " → " <name> "  (any case)
//   3. Possessives "your "/"Your "/"yours" → "<name>'s " / "<name>'s"
//   4. Reflexive "yourself" → "themself"
//
// Use this any time the host's own avatar messages need to be presented to
// the OTHER player from a third-person view — typically when forwarding the
// host's "You X" messages to the client.  Without the possessive pass, the
// client sees "FrozenFoxy drops your X" which reads as "the host dropped
// MY (client's) X" — semantically wrong.
static void mp_rewrite_first_to_third( std::string &s, const std::string &subject )
{
    if( subject.empty() ) {
        return;
    }
    const std::string possessive = subject + "'s";

    auto replace_all = []( std::string & dst, const std::string & from, const std::string & to ) {
        if( from.empty() ) {
            return;
        }
        size_t p = 0;
        while( ( p = dst.find( from, p ) ) != std::string::npos ) {
            dst.replace( p, from.size(), to );
            p += to.size();
        }
    };

    // Possessives first — "your" before "you" so a later " you " sub doesn't
    // chew the start of "your".
    replace_all( s, " your ", " " + possessive + " " );
    replace_all( s, " Your ", " " + possessive + " " );
    replace_all( s, " yours ", " " + possessive + " " );
    replace_all( s, " yourself ", " themself " );
    if( s.size() >= 5 && s.compare( 0, 5, "Your " ) == 0 ) {
        s.replace( 0, 4, possessive );
    }

    // Mid-string "you" (lowercase, e.g., "the can hit you in the head") becomes
    // the subject name.
    replace_all( s, " you ", " " + subject + " " );

    // Leading "You " — handled last so the verb conjugation pass below can
    // assume the subject already sits at position 0.
    if( s.size() >= 4 && s.compare( 0, 4, "You " ) == 0 ) {
        s.replace( 0, 3, subject );
        // Now conjugate the verb that follows (e.g., "drop" → "drops").
        const size_t vs = subject.size() + 1; // skip "<name> "
        size_t ve = s.find( ' ', vs );
        if( ve == std::string::npos ) {
            ve = s.size();
        }
        if( ve > vs + 1 ) {
            const std::string v = s.substr( vs, ve - vs );
            std::string fixed;
            // Irregular verbs (second-person plural → third-person singular).
            // Must run before the generic "add s" rule below, otherwise
            // "have" → "haves", "do" → "dos", "are" → "ares".
            static const std::vector<std::pair<std::string, std::string>> irregulars = {
                { "have", "has" }, { "do", "does" }, { "are", "is" }, { "were", "was" },
                { "go", "goes" }, { "say", "says" }, { "Have", "has" }, { "Do", "does" },
                { "Are", "is" }, { "Were", "was" },
            };
            bool matched = false;
            for( const auto &p : irregulars ) {
                if( v == p.first ) {
                    fixed = p.second;
                    matched = true;
                    break;
                }
            }
            auto ends_with = [&]( const char *suf, size_t n ) {
                return v.size() >= n && v.compare( v.size() - n, n, suf ) == 0;
            };
            if( !matched ) {
                if( ends_with( "y", 1 ) && v.size() > 1 ) {
                    // try → tries (but not "stay" → "stays" — vowel before 'y' keeps it)
                    const char before_y = v[v.size() - 2];
                    const bool vowel_before_y = before_y == 'a' || before_y == 'e' ||
                                                before_y == 'i' || before_y == 'o' || before_y == 'u';
                    fixed = vowel_before_y ? v + "s" : v.substr( 0, v.size() - 1 ) + "ies";
                } else if( ends_with( "s", 1 ) || ends_with( "x", 1 ) || ends_with( "z", 1 ) ||
                           ends_with( "ch", 2 ) || ends_with( "sh", 2 ) ) {
                    fixed = v + "es";
                } else {
                    fixed = v + "s";
                }
            }
            s.replace( vs, ve - vs, fixed );
        }
    }
}

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

    // Irregular verbs (third-person singular → second-person plural).  These
    // must be handled before the generic "trim trailing s" rule, otherwise
    // "has" → "ha", "does" → "doe", "is" → "i".
    static const std::vector<std::pair<std::string, std::string>> irregulars = {
        { "has", "have" }, { "does", "do" }, { "is", "are" }, { "was", "were" },
        { "goes", "go" }, { "says", "say" }, { "Has", "have" }, { "Does", "do" },
        { "Is", "are" }, { "Was", "were" },
    };
    bool matched = false;
    for( const auto &p : irregulars ) {
        if( v == p.first ) {
            fixed = p.second;
            matched = true;
            break;
        }
    }
    auto ends_with = [&]( const char *suffix, size_t n ) {
        return v.size() >= n && v.compare( v.size() - n, n, suffix ) == 0;
    };
    if( !matched ) {
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

void host_broadcast_vehicle_step()
{
    if( !is_hosting() || !remote_player_connected ) {
        return;
    }
    server *srv = get_active_server();
    if( !srv ) {
        return;
    }
    map &vmap = get_map();
    std::string vehicles_json = "[";
    bool vfirst = true;
    for( const wrapped_vehicle &wv : vmap.get_vehicles() ) {
        vehicle *v = wv.v;
        if( !v ) {
            continue;
        }
        auto vid_it = g_server_veh_ids.find( v );
        if( vid_it == g_server_veh_ids.end() ) {
            g_server_veh_ids[v] = ++g_next_net_id;
            vid_it = g_server_veh_ids.find( v );
        }
        const uint32_t nid = vid_it->second;
        const tripoint_abs_ms vabs = v->pos_abs();
        const int face_deg     = static_cast<int>( std::lround( to_degrees( v->face.dir() ) ) );
        const int turn_dir_deg = static_cast<int>( std::lround( to_degrees( v->turn_dir ) ) );
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
                         + ",\"cruise\":" + std::to_string( v->cruise_velocity ) + "}";
    }
    vehicles_json += "]";
    // Suppress identical back-to-back broadcasts.  vehproceed() fires this hook
    // once per call even for stationary vehicles whose of_turn is being touched
    // by act_on_map(), which floods the client with dozens of duplicate packets
    // per host turn and stretches the grant→ack cycle to ~500ms.  Same
    // baseline-diff pattern as the cargo broadcast.
    static std::string g_last_vehicle_step_payload;
    if( vehicles_json == g_last_vehicle_step_payload ) {
        return;
    }
    g_last_vehicle_step_payload = vehicles_json;
    srv->post_broadcast( "{\"type\":\"vehicle_step\",\"vehicles\":" + vehicles_json + "}\n" );
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
        // Convert first-person to third-person properly: subject substitution,
        // verb conjugation ("drop" → "drops"), AND possessive substitution
        // ("your X" → "<name>'s X").  Without the possessive pass the client
        // sees "<host> drops your X" and reads "your" as referring to itself
        // — wrong subject.
        mp_rewrite_first_to_third( out, host_name );
        mp_log( "[cdda-mp] host_combat_msg: " + out );
        g_host_action_msgs_pending.push_back( out );
    }
}

void host_broadcast_post_action()
{
    if( !is_hosting() || !remote_player_connected ) {
        return;
    }
    server *srv = get_active_server();
    if( !srv ) {
        return;
    }
    mp_log( "[cdda-mp] HOST-ACK: post-action broadcast grant_seq="
            + std::to_string( g_grant_seq ) );
    srv->post_broadcast( serialize_remote_player_state() + "\n" );
}

// Standard turn-ending broadcast for handlers in handle_remote_action.
//
// All actions that consume the client's full turn budget MUST end with this
// (or the explicit equivalent): zero g_remote_moves, set the acted flag, log
// SRV-ACK with the current grant_seq, and broadcast the new state so the
// client receives a moves=0 ack and clears its ack-guard.
//
// The earlier "g_remote_moves -= remote->get_speed()" pattern relied on speed
// equalling the grant amount; that's true today but fragile (encumbrance,
// effects, mutations can change speed mid-turn).  Setting moves to exactly 0
// removes the coupling entirely.  push_partner's "-= 20" was the canary that
// exposed the latent bug: subtracting less than the grant left moves > 0 and
// the client interpreted the broadcast as a new grant instead of an ack-clear,
// wedging lockstep.
static void srv_emit_ack( const char *action_name )
{
    g_client_acted_this_turn = true;
    g_remote_moves = 0;
    mp_log( std::string( "[cdda-mp] SRV-ACK: moves=0 (" ) + action_name
            + ") grant_seq=" + std::to_string( g_grant_seq ) );
    server *srv = get_active_server();
    if( srv ) {
        srv->post_broadcast( serialize_remote_player_state() + "\n" );
    }
    // The handler just mutated authoritative world state (positions, items,
    // doors, vehicle flags, etc.) but the host may be sitting in a blocking
    // wait_for_client_action poll where no normal turn boundary triggers a
    // redraw. Force the main UI adaptor to redraw so swap/push/pickup/drop/
    // open-close/etc. show up immediately on the host instead of lagging
    // until the next turn cycle.
    g->invalidate_main_ui_adaptor();
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

    // Track the client's current activity for HUD + partner-notice display
    // AND for the lockstep bypass in wait_for_client_action.  Field is empty
    // when the client is idle.  The heartbeat is belt-and-suspenders: the
    // primary signal is the explicit activity_start / activity_end actions
    // handled below, but every action carries this snapshot too so a missed
    // start/end can still be reconciled on the next packet.
    if( jo.has_string( "client_activity" ) ) {
        g_partner_activity = jo.get_string( "client_activity" );
        mp_partner_activity_transition_check();
    }
    if( jo.has_int( "client_activity_pct" ) ) {
        g_partner_activity_pct = jo.get_int( "client_activity_pct" );
    }
    if( jo.has_int( "client_activity_moves_total" ) ) {
        g_partner_activity_moves_total = jo.get_int( "client_activity_moves_total" );
    }
    if( jo.has_int( "client_calendar_turn" ) ) {
        g_partner_calendar_turn = jo.get_int( "client_calendar_turn" );
    }

    // Explicit lifecycle markers for the client's passive activities.  These
    // are signal-only — they don't consume host moves and don't drive any
    // simulation on the host side.  Their sole job is to open and close the
    // lockstep bypass cleanly, so variable-duration activities like ACT_DROP
    // don't race the heartbeat.
    if( msg.find( "\"action\":\"activity_start\"" ) != std::string::npos ) {
        const std::string id = jo.get_string( "activity_id", "" );
        const std::string prev = g_partner_activity;
        if( !id.empty() ) {
            g_partner_activity = id;
            mp_partner_activity_transition_check();
        }
        mp_log( "[cdda-mp] ACT-START RECV: id=" + id
                + " g_partner_activity prev=" + prev + " now=" + g_partner_activity );
        return;
    }
    if( msg.find( "\"action\":\"activity_end\"" ) != std::string::npos ) {
        const std::string id = jo.get_string( "activity_id", "" );
        const std::string prev = g_partner_activity;
        g_partner_activity.clear();
        mp_partner_activity_transition_check();
        mp_log( "[cdda-mp] ACT-END RECV: id=" + id
                + " g_partner_activity prev=" + prev + " now=" + g_partner_activity );
        return;
    }

    // Graceful session-end notification from the client (v1 save handshake).
    // Snapshot the host's world before the TCP socket goes down so any
    // shared state the client touched is captured.  Client owns its own
    // .sav, so nothing to do for their character on this side.
    if( msg.find( "\"action\":\"session_ending\"" ) != std::string::npos ) {
        mp_log( "[cdda-mp] SESSION-END RECV: client is leaving — auto-saving host" );
        add_msg( m_warning, _( "Your partner is leaving.  The game has been saved." ) );
        g->quicksave();
        return;
    }

    // Templates wire-sync handlers (symmetric — same shape on host and client).
    if( msg.find( "\"type\":\"templates_list\"" ) != std::string::npos ) {
        mp_handle_templates_list( msg );
        return;
    }
    if( msg.find( "\"type\":\"template_request\"" ) != std::string::npos ) {
        mp_handle_template_request( msg );
        return;
    }
    if( msg.find( "\"type\":\"template_data\"" ) != std::string::npos ) {
        mp_handle_template_data( msg );
        return;
    }

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

    // Client-side avatar messages forwarded for display on the host's log.
    // The client has already substituted "You" with their character name, so
    // we just need to add_msg each verbatim.  Lets the host see notifications
    // like "Roy is now reading [Adventure Novel], 5 to stop early."
    if( jo.has_array( "client_msgs" ) ) {
        for( const JsonValue &mv : jo.get_array( "client_msgs" ) ) {
            const std::string text = mv.get_string();
            add_msg( m_info, text );
        }
        // Loop-break: messages forwarded FROM the client must not be picked
        // up by the host's between-action forwarder (NPC-name substitution
        // path in serialize_remote_player_state) and sent back as msgs.
        // Otherwise we get an infinite ping-pong of the same notification.
        g_last_forwarded_msg_count = Messages::size();
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
            std::string ma_style_str;
            jo.read( "ma_style", ma_style_str );
            if( !ma_style_str.empty() ) {
                const matype_id mid( ma_style_str );
                if( mid.is_valid() ) {
                    remote->martial_arts_data->set_style( mid, true );
                    mp_log( "[cdda-mp] worn_sync: set ma_style=" + ma_style_str );
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
                    bool touched = false;
                    if( to.has_string( "ter" ) ) {
                        const ter_id tid( to.get_string( "ter" ) );
                        if( tid.id().is_valid() ) {
                            m.ter_set( bub, tid );
                            touched = true;
                        }
                    }
                    if( to.has_string( "furn" ) ) {
                        const furn_id fid( to.get_string( "furn" ) );
                        if( fid.id().is_valid() ) {
                            m.furn_set( bub, fid );
                            touched = true;
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
                        touched = true;
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
                                touched = true;
                            }
                        }
                    }
                    if( to.has_string( "trap" ) ) {
                        const std::string trap_str = to.get_string( "trap" );
                        // Skip if the terrain has a built-in trap (e.g. downspout
                        // funnel on t_gutter_downspout); trap_set refuses any trap
                        // on such tiles and debugmsgs.  The peer derives the trap
                        // from the terrain, so no work needed.
                        const trap_id &builtin = m.ter( bub )->trap;
                        if( builtin != tr_null ) {
                            // no-op
                        } else if( trap_str.empty() || trap_str == "tr_null" ) {
                            m.trap_set( bub, tr_null );
                        } else {
                            const trap_str_id tsid( trap_str );
                            if( tsid.is_valid() ) {
                                m.trap_set( bub, tsid.id() );
                            }
                        }
                        touched = true;
                    }
                    if( to.has_string( "graffiti" ) ) {
                        const std::string gtext = to.get_string( "graffiti" );
                        if( gtext.empty() ) {
                            m.delete_graffiti( bub );
                        } else {
                            m.set_graffiti( bub, gtext );
                        }
                        touched = true;
                    }
                    // Refresh baseline to the just-applied state so the next
                    // build_tile_changes broadcast doesn't echo this same tile
                    // back to the client.  Erase-and-resend was causing the
                    // partner's ACT_WAIT heartbeat (which re-includes every
                    // visible tile) to ping-pong ~80 KB of items+fields per
                    // turn, blocking the host's input pump.
                    if( touched ) {
                        g_tile_baseline[abs] = compute_tile_state( abs );
                    }
                }
            }
        } catch( const JsonError & ) {}
    }

    // Apply vehicle cargo changes the client made (items placed in trunks,
    // freezers, lockers via SP drop_activity_actor → put_into_vehicle_or_drop).
    // Mirrors client_tile_changes for items stored inside vehicle parts.
    if( msg.find( "\"client_veh_cargo_changes\":" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            if( jo.has_array( "client_veh_cargo_changes" ) ) {
                map &m = get_map();
                for( const JsonValue &entry : jo.get_array( "client_veh_cargo_changes" ) ) {
                    JsonObject co = entry.get_object();
                    co.allow_omitted_members();
                    const tripoint_abs_ms vp_abs{
                        co.get_int( "x" ), co.get_int( "y" ), co.get_int( "z" )
                    };
                    if( !m.inbounds( vp_abs ) ) {
                        continue;
                    }
                    const tripoint_bub_ms vp_bub = m.get_bub( vp_abs );
                    const std::optional<vpart_reference> cargo_vp = m.veh_at( vp_bub ).cargo();
                    if( !cargo_vp ) {
                        mp_log( "[cdda-mp] server veh cargo miss: no cargo part at " +
                                std::to_string( vp_abs.x() ) + "," +
                                std::to_string( vp_abs.y() ) + "," +
                                std::to_string( vp_abs.z() ) );
                        continue;
                    }
                    vehicle &veh = cargo_vp->vehicle();
                    vehicle_part &part = cargo_vp->part();
                    mp_log( "[cdda-mp] server apply veh cargo @ " +
                            std::to_string( vp_abs.x() ) + "," +
                            std::to_string( vp_abs.y() ) + "," +
                            std::to_string( vp_abs.z() ) );
                    {
                        vehicle_stack stack = veh.get_items( part );
                        while( !stack.empty() ) {
                            stack.erase( stack.begin() );
                        }
                    }
                    if( co.has_array( "items" ) ) {
                        for( const JsonValue &iv : co.get_array( "items" ) ) {
                            try {
                                item new_item;
                                JsonObject io = iv.get_object();
                                io.allow_omitted_members();
                                new_item.deserialize( io );
                                if( !new_item.typeId().is_empty() &&
                                    new_item.typeId().is_valid() ) {
                                    veh.add_item( m, part, new_item );
                                }
                            } catch( const JsonError & ) {}
                        }
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

    // Partner-menu swap: swap host's avatar with the client's proxy NPC.
    // Triggered when the client picked "Swap positions" from the bump menu.
    if( msg.find( "\"action\":\"swap_with_partner\"" ) != std::string::npos ) {
        avatar &host_av = get_avatar();
        const tripoint_bub_ms host_pre  = host_av.pos_bub();
        const tripoint_bub_ms proxy_pre = remote->pos_bub();
        mp_log( "[cdda-mp] SRV-SWAP-PRE: host=" + host_pre.to_string() +
                " proxy=" + proxy_pre.to_string() );
        add_msg( _( "%s swaps places with you." ), remote->get_name() );
        g->swap_critters( host_av, *remote );
        mp_log( "[cdda-mp] SRV-SWAP-POST: host=" + host_av.pos_bub().to_string() +
                " proxy=" + remote->pos_bub().to_string() );
        srv_emit_ack( "swap_with_partner" );
        return;
    }

    // Partner-menu push: host's avatar moves one tile away from the client's
    // proxy.  Triggered when the client picked "Push away" from the bump menu.
    // avatar has no move_away_from() (that's npc-only), so compute the step
    // direction manually: sign of (host - proxy) on each axis.
    if( msg.find( "\"action\":\"push_partner\"" ) != std::string::npos ) {
        avatar &host_av = get_avatar();
        const tripoint_bub_ms host_pos  = host_av.pos_bub();
        const tripoint_bub_ms proxy_pos = remote->pos_bub();
        mp_log( "[cdda-mp] SRV-PUSH-PRE: host=" + host_pos.to_string() +
                " proxy=" + proxy_pos.to_string() );
        const int dx = ( host_pos.x() > proxy_pos.x() ) ? 1
                       : ( host_pos.x() < proxy_pos.x() ) ? -1 : 0;
        const int dy = ( host_pos.y() > proxy_pos.y() ) ? 1
                       : ( host_pos.y() < proxy_pos.y() ) ? -1 : 0;
        const tripoint_bub_ms target = host_pos + tripoint_rel_ms( dx, dy, 0 );
        if( ( dx != 0 || dy != 0 ) && !m.impassable( target ) ) {
            host_av.setpos( m, target );
            add_msg( _( "%s pushes you out of the way." ), remote->get_name() );
        } else {
            add_msg( m_warning, _( "%s tries to push you but you have nowhere to go." ),
                     remote->get_name() );
        }
        mp_log( "[cdda-mp] SRV-PUSH-POST: host=" + host_av.pos_bub().to_string() +
                " proxy=" + remote->pos_bub().to_string() );
        srv_emit_ack( "push_partner" );
        return;
    }

    // Partner-menu tap-on-shoulder: client interrupts host's "wait for
    // several minutes" activity. Only cancels ACT_WAIT — other activities
    // (sleep, crafting, reading) are intentionally not interruptible by tap
    // at this stage. If host isn't waiting, the tap is acknowledged but the
    // activity state is left alone.
    if( msg.find( "\"action\":\"tap_partner\"" ) != std::string::npos ) {
        avatar &host_av = get_avatar();
        static const activity_id s_act_wait( "ACT_WAIT" );
        const bool was_waiting = host_av.activity.id() == s_act_wait;
        mp_log( "[cdda-mp] SRV-TAP-PRE: was_waiting=" + std::to_string( was_waiting ) +
                " activity=" + host_av.activity.id().str() );
        if( was_waiting ) {
            host_av.cancel_activity();
            add_msg( _( "%s taps you on the shoulder, snapping you out of your wait." ),
                     remote->get_name() );
        } else {
            add_msg( _( "%s taps you on the shoulder." ), remote->get_name() );
        }
        srv_emit_ack( "tap_partner" );
        return;
    }

    // Wait — drain one turn's worth of AP.
    const bool is_wait = msg.find( "\"action\":\"wait\"" ) != std::string::npos ||
                         msg.find( "\"action\": \"wait\"" ) != std::string::npos;
    if( is_wait ) {
        mp_log( "[cdda-mp] wait recv: ctrl_veh=" + std::to_string( remote->controlling_vehicle ) +
                " g_remote_moves=" + std::to_string( g_remote_moves ) );
        srv_emit_ack( "wait" );
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
        srv_emit_ack( "pickup" );
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
        srv_emit_ack( "drop" );
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
        srv_emit_ack( is_open ? "open" : "close" );
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
        // Inlined srv_emit_ack: smash needs custom state injection for the
        // bash animation, so the broadcast is custom rather than the helper.
        g_client_acted_this_turn = true;
        g_remote_moves = 0;
        mp_log( "[cdda-mp] SRV-ACK: moves=0 (smash) grant_seq=" +
                std::to_string( g_grant_seq ) );
        flush_action_msgs( pre_action_msg, remote->name );
        server *srv = get_active_server();
        if( srv ) {
            std::string state = serialize_remote_player_state();
            state = state.substr( 0, state.size() - 1 )
                    + ",\"smash_result\":\"" + smash_result_str + "\""
                    + ",\"smash_x\":" + std::to_string( smash_target_abs.x() )
                    + ",\"smash_y\":" + std::to_string( smash_target_abs.y() )
                    + ",\"smash_z\":" + std::to_string( smash_target_abs.z() ) + "}";
            srv->post_broadcast( state + "\n" );
        }
        // See NPC-MOVE invalidate for rationale.
        g->invalidate_main_ui_adaptor();
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
        srv_emit_ack( "position_sync" );
        return;
    }

    // Eat / use item — client is authoritative over its own nutrition; server just
    // drains one turn of AP and re-broadcasts state so the client's HUD stays current.
    if( msg.find( "\"action\":\"eat\"" ) != std::string::npos ) {
        srv_emit_ack( "eat" );
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
                // board_vehicle early-returns if proxy is already seated at this part,
                // so this is safe to call unconditionally.  Without it, a proxy whose
                // in_vehicle was set by the per-turn position sync (rather than by a
                // prior board_vehicle) would never get passenger_flag on the seat.
                here.board_vehicle( bub, remote );
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
        srv_emit_ack( "control_vehicle" );
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
            g_remote_moves = 0;
        }
        // Mirror SP's pldrive: turning costs AP but doesn't end the turn unless
        // AP runs out.  If the proxy still has moves, send a partial-turn update
        // (free=true) so the client can do more driving inputs (more turns, cruise
        // changes, pause to commit) in the same turn — matching SP behavior where
        // the driver can chain inputs until moves reach 0.
        const bool turn_ended = g_remote_moves <= 0;
        if( turn_ended ) {
            g_client_acted_this_turn = true;
            g_remote_moves = 0;
            mp_log( "[cdda-mp] SRV-ACK: moves=0 (pldrive) grant_seq=" +
                    std::to_string( g_grant_seq ) );
        }
        flush_action_msgs( pre_action_msg, remote->name );
        server *srv = get_active_server();
        if( srv ) {
            std::string state = serialize_remote_player_state();
            if( !turn_ended ) {
                state = state.substr( 0, state.size() - 1 ) + ",\"free\":true}";
            }
            srv->post_broadcast( state + "\n" );
        }
        // See NPC-MOVE invalidate for rationale.
        g->invalidate_main_ui_adaptor();
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
        // See NPC-MOVE invalidate for rationale.
        g->invalidate_main_ui_adaptor();
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
        g_last_forwarded_msg_count = Messages::size();
        srv_emit_ack( "cruise" );
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
        g_last_forwarded_msg_count = Messages::size();
        srv_emit_ack( "stop_engine" );
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
        g_last_forwarded_msg_count = Messages::size();
        srv_emit_ack( "toggle_engine" );
        return;
    }

    // Honk horn.
    // Client → host vehicle construction sync.  Client runs the activity
    // locally (timer, moves, items, messages, local vehicle mutation) and on
    // finish dispatches this action with the actor's serialized state.  Host
    // reconstructs the actor and runs complete_vehicle against the proxy NPC's
    // crafting inventory and the host's authoritative vehicle.  Piece A's
    // parts-count-change detection then triggers a snapshot rebroadcast so
    // the client's local vehicle is replaced with the post-construction
    // snapshot — covers install, remove (including appliance), repair,
    // refill, change-shape paths in vehicle_activity_actor.
    if( msg.find( "\"action\":\"vehicle_construct\"" ) != std::string::npos ) {
        try {
            JsonValue actor_jv = jo.get_member( "actor" );
            std::unique_ptr<activity_actor> actor =
                vehicle_activity_actor::deserialize( actor_jv );
            if( !actor ) {
                mp_log( "[cdda-mp] vehicle_construct: actor deserialize returned null" );
            } else {
                player_activity tmp_act( *actor );
                // complete_vehicle mutates the actor's internal state (vp_index
                // adjustments etc.) so call it on the deserialized instance
                // rather than the temp player_activity's clone.
                static_cast<vehicle_activity_actor *>( actor.get() )
                    ->complete_vehicle( tmp_act, *remote );
                mp_log( "[cdda-mp] HOST-VEH-CONSTRUCT applied for proxy NPC" );
            }
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] vehicle_construct parse error: " + std::string( e.what() ) );
        }
        flush_action_msgs( pre_action_msg, remote->name );
        srv_emit_ack( "vehicle_construct" );
        return;
    }

    if( msg.find( "\"action\":\"honk\"" ) != std::string::npos ) {
        map &here = get_map();
        const tripoint_bub_ms bub = remote->pos_bub();
        if( const optional_vpart_position vp = here.veh_at( bub ) ) {
            vehicle &veh = vp->vehicle();
            veh.honk_horn( here );
        }
        flush_action_msgs( pre_action_msg, remote->name );
        srv_emit_ack( "honk" );
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
            // The client's local bump check (handle_action.cpp) is the intended
            // UX path — it opens the partner menu locally before the move ever
            // dispatches.  If that check missed (position-sync drift, etc.) we
            // land here.  Treat the bump as a consumed turn so the host's
            // wait_for_client_action releases cleanly.
            srv_emit_ack( "bump_host_fallthrough" );
            return;
        }
        const shared_ptr_fast<monster> target = get_creature_tracker().find( next_abs );
        if( target ) {
            // melee_attack() charges moves on the NPC internally; capture the result.
            remote->melee_attack( *target, true );
            g_remote_moves = remote->get_moves();
            acted = true;
        } else if( !m.impassable( next ) ) {
            // Mirror avatar_action::move boarding semantics: unboard from current
            // vehicle tile before setpos, then board at the new tile if boardable.
            // Without this, board_vehicle is never called on the way in and the
            // controls seat's passenger_flag stays clear, which makes the host
            // flag the vehicle as unmanned and trigger spontaneous skids.
            // Guard the unboard call: in_vehicle can be stale (set true by the
            // per-turn sync or carried across a save/load) while the proxy
            // isn't actually standing on a vehicle.  Calling unboard_vehicle
            // at a non-vehicle tile fires a debugmsg.
            if( remote->in_vehicle ) {
                if( m.veh_at( remote->pos_bub() ).part_with_feature( "BOARDABLE", true ) ) {
                    m.unboard_vehicle( remote->pos_bub() );
                } else {
                    remote->in_vehicle = false;
                    remote->controlling_vehicle = false;
                }
            }
            remote->setpos( m, next );
            if( m.veh_at( remote->pos_bub() ).part_with_feature( "BOARDABLE", true ) ) {
                m.board_vehicle( remote->pos_bub(), remote );
            }
            mp_log( "[cdda-mp] NPC-MOVE: setpos done, pos_abs=" + std::to_string( remote->pos_abs().x() ) +
                    "," + std::to_string( remote->pos_abs().y() ) +
                    " bub=" + std::to_string( remote->pos_bub().x() ) + "," + std::to_string( remote->pos_bub().y() ) +
                    " in_vehicle=" + std::to_string( remote->in_vehicle ) );
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
            // No explicit AP charge here — the canonical SRV-ACK below zeros the
            // move budget when acted=true.
            if( m.open_door( *remote, next, true, false ) ) {
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
        mp_log( "[cdda-mp] SRV-ACK: sending moves=0 ack to client, grant_seq=" + std::to_string( g_grant_seq ) );
    } else {
        mp_log( "[cdda-mp] SRV-FREE: no-op move (wall/bump), sending free=true, moves=" + std::to_string( g_remote_moves ) );
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

    // Mirror srv_emit_ack's UI invalidation — the move handler inlines its own
    // broadcast (above) so it doesn't go through srv_emit_ack, but the
    // authoritative position just changed and the host may be sitting in a
    // blocking wait_for_client_action poll with no normal turn boundary to
    // trigger a redraw.  Without this, the proxy NPC's new tile only appears
    // after an unrelated event (alt-tab, host keypress, monmove) forces the
    // window to repaint.
    g->invalidate_main_ui_adaptor();
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
        // If we previously had a live proxy and it's now gone, treat it as a
        // death (monsters on the host's side killed the NPC representing the
        // client).  Notify the client cleanly so they get a "You died." flow
        // instead of a silent "Lost connection to server" 20 seconds later
        // when something else times out.  Then tear down the host-side state.
        if( g_proxy_was_alive ) {
            mp_log( "[cdda-mp] PROXY-DIED: notifying client and disconnecting" );
            if( server *srv = get_active_server() ) {
                srv->post_broadcast( "{\"type\":\"you_died\"}\n" );
                // Brief flush window so the packet leaves before the socket
                // is torn down — same pattern as notify_client_host_died.
                std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
            }
            g_proxy_was_alive = false;
            remove_remote_player();
        }
        return;
    }
    g_proxy_was_alive = true;
    g_client_acted_this_turn = false;
    g_remote_moves = remote->get_speed();
    ++g_grant_seq;
    const player_activity &ha = get_avatar().activity;
    mp_log( "[cdda-mp] grant_client_turn: remote_moves=" + std::to_string( g_remote_moves ) +
            " seq=" + std::to_string( g_grant_seq ) +
            " host_act=" + ( ha ? ha.id().str() : "none" ) );
    // Proxy skips npcmove so never auto-regenerates stamina. Replicate the
    // update_body() path that the real avatar gets each game turn.
    remote->update_stamina( 1 );
    check_separation_warning( get_avatar().pos_abs(), remote->pos_abs() );
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

    // Sleep is the one true fast-forward case: 28800 ticks of strict lockstep
    // would mean ~48 minutes wall-clock for 8 hours of game-sleep.  Host-side
    // sleep effect bypasses the lockstep wait so the host can race through
    // those turns at native do_turn speed.  Client-side sleep needs its own
    // bypass once implemented — for now sleep is treated as host-only.
    {
        static const efftype_id eff_sleep( "sleep" );
        if( get_avatar().has_effect( eff_sleep ) ) {
            process_mp_events();
            mp_log( "[cdda-mp] lockstep-skip: host_act=sleep" );
            return;
        }
    }

    // Both-passive fast-forward: when both sides are in passive activities
    // (both crafting, both eating, host crafting + client helping, etc.),
    // drop strict lockstep so wall-clock progress matches SP's machine-speed
    // activity ticks.  20-min in-game craft → ~5-10s real-time instead of
    // ~6.5min under 1:1 lockstep.  SP's per-turn activity cancellation
    // (hostile-in-sight, low HP, sound, etc.) fires on the host as turns
    // advance and naturally exits FF: cancel ends av_act, next call returns
    // false, the following turn re-enters strict lockstep.
    static bool ff_was_active = false;
    const bool ff_now = should_fast_forward();
    if( ff_now != ff_was_active ) {
        if( ff_now ) {
            mp_log( std::string( "[cdda-mp] lockstep-skip: FAST-FORWARD enter host_act=" )
                    + ( get_avatar().activity ? get_avatar().activity.id().str() : "?" )
                    + " partner_act=" + g_partner_activity );
        } else {
            mp_log( "[cdda-mp] lockstep-resume: FAST-FORWARD exit" );
        }
        ff_was_active = ff_now;
    }
    if( ff_now ) {
        process_mp_events();
        return;
    }

    // No partner-activity bypass: every activity (drop, wear, take_off, read,
    // craft, eat, wait, …) now dispatches a "wait" ack per tick via the
    // do_turn post-loop dispatch path, so strict lockstep works for them all.
    // The earlier g_partner_activity bypass was leftover scaffolding from the
    // DISCONNECT-TIMEOUT era and would let the host fast-forward freely while
    // the partner was inside an activity that took even a few packets to
    // complete (drop on a seat → host's |-wait fast-forwarded ~30 minutes).

    g_host_waiting_for_client = true;
    const auto t_start = std::chrono::steady_clock::now();
    // Pure lockstep: every game-turn requires a client ack — including turns
    // the host spends inside a long activity (|-wait, sleep, craft).  The
    // activity progresses one game-turn at a time, paced by the client's
    // round-trip.  This keeps the shared calendar consistent: time only
    // advances when both ends agree on an action for this turn.
    //
    // No wall-clock timeout: in turn-based, "wait indefinitely" is correct.
    // TCP connection drop (remote_player_connected = false) is the only
    // legitimate exit besides the client acting.  Each iter caps at 16ms
    // so SDL stays pumped during long waits.
    const bool host_in_wait = host_is_in_wait_activity();  // logged only
    {
        const player_activity &ha_enter = get_avatar().activity;
        mp_log( "[cdda-mp] SRV-WAIT: entering, grant_seq=" + std::to_string( g_grant_seq ) +
                " host_in_wait=" + std::to_string( host_in_wait ) +
                " host_act=" + ( ha_enter ? ha_enter.id().str() : "none" ) );
    }

    int iter_count = 0;
    // Per-phase max times across this SRV-WAIT, logged once on exit so we
    // don't spam per-iter.  Anything that spikes above ~16ms is blocking the
    // SDL input pump and explains "I pressed zoom and it didn't react."
    int max_waitev_ms = 0;
    int max_drain_ms  = 0;
    int max_pump_ms   = 0;
    int max_redraw_ms = 0;
    int max_input_ms  = 0;
    // Track host activity across iters so we can detect a transition (cancel
    // mid-handle_action, or a new activity started somehow) and force-broadcast
    // it.  Without this, the client's view of g_partner_activity goes stale —
    // it FF-decides based on what we LAST told it, not what we're doing now.
    // If host's activity ended after the iteration's grant_client_turn
    // broadcast but before reaching the wait, the client still thinks FF
    // applies, suppresses its wait dispatch, and we deadlock waiting for an
    // ack that won't come.  Cheap to do — only broadcasts on actual edge.
    std::string prev_wait_host_act = get_avatar().activity
                                     ? get_avatar().activity.id().str() : "";
    while( remote_player_connected ) {
        if( g_client_acted_this_turn ) {
            break;  // client acted, advance shared clock by this turn
        }
        // Mid-wait FF check: client's activity_start may arrive *after* we
        // entered the wait (host entered first), so the entry-time
        // should_fast_forward() returned false.  Re-check each iter — the
        // moment partner activity flips to passive while ours already is,
        // bail out and let do_turn race through both crafts at SP speed.
        // Without this, the client (also in FF) won't dispatch waits, so
        // g_client_acted_this_turn never flips and we deadlock both ends.
        if( should_fast_forward() ) {
            mp_log( "[cdda-mp] SRV-WAIT: FAST-FORWARD engaged mid-wait, bailing" );
            break;
        }
        // Host-activity transition broadcast: if our activity changed since
        // entering the wait (typically: just cancelled mid-handle_action),
        // push fresh state to the client right away.  Otherwise the client
        // keeps thinking we're still crafting and stays in FF — meaning it
        // never dispatches a wait — meaning this wait never unblocks.
        const std::string cur_host_act = get_avatar().activity
                                         ? get_avatar().activity.id().str() : "";
        if( cur_host_act != prev_wait_host_act ) {
            mp_log( std::string( "[cdda-mp] SRV-WAIT: host activity transition " ) +
                    prev_wait_host_act + " -> " + cur_host_act +
                    ", broadcasting state" );
            if( server *srv = get_active_server() ) {
                srv->post_broadcast( serialize_remote_player_state() + "\n" );
            }
            prev_wait_host_act = cur_host_act;
        }
        // Cap each iteration at ~16ms so SDL gets pumped at 60Hz.  Without this
        // the main thread blocks in TCP recv indefinitely, which trips the macOS
        // spinning-beachball watchdog.  mp_poll_input() (handle_action) is
        // intentionally NOT called: it blocks waiting for a keypress, eating the
        // async-tick budget by up to seconds.  Re-adding host-side menu access
        // during waits will need a non-blocking input peek, not a full action
        // dispatcher.
        // Burst mode: both players in non-interactive activities (e.g. crafting +
        // helping).  Drop the 16ms throttle so the shared clock advances as fast
        // as the host's CPU can serialize ticks — turns an 8-hour craft from
        // minutes of staring at the wait popup into seconds.
        const auto step = mp_in_burst_mode()
                          ? std::chrono::milliseconds( 0 )
                          : std::chrono::milliseconds( 16 );
        const auto t_iter0 = std::chrono::steady_clock::now();
        get_mp_queue().wait_for_event( step );
        const auto t_after_wait = std::chrono::steady_clock::now();
        process_mp_events();
        const auto t_after_drain = std::chrono::steady_clock::now();
        ensure_mp_hud();
        inp_mngr.pump_events();
        const auto t_after_pump = std::chrono::steady_clock::now();
        // Redraw the side strip + Co-op panel ~10x/sec while we're blocked so
        // the host's HUD actually flips to red while locked, instead of staying
        // green until the wait exits.  Rate-limited because ui_manager::redraw
        // isn't free and 60Hz redraws are wasteful when nothing visually changes.
        static auto last_redraw = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if( std::chrono::duration_cast<std::chrono::milliseconds>( now - last_redraw ).count() > 100 ) {
            ui_manager::redraw();
            last_redraw = now;
        }
        const auto t_after_redraw = std::chrono::steady_clock::now();
        // Mirror the client's locked-input branch: call full handle_action so
        // every free UI action (zoom, morale, map, inventory, messages, look)
        // works while the host is waiting for the client.  handle_action gates
        // moves-consuming actions out via the host-locked check at
        // handle_action.cpp:3038, so this is safe — only pure UI passes.
        //
        // handle_action blocks on a keypress, but its internal TIMEOUT poll
        // calls process_mp_events for the host every ~125ms, and the
        // client_just_acted TIMEOUT escape in get_player_input breaks out of
        // the input poll the moment the client acts.  Net result: host gets
        // full SP-style input access AND the wait still exits on client ack.
        if( !get_avatar().activity ) {
            g->mp_poll_input();
        } else {
            // In-activity (|-wait, craft, etc.): keep the legacy 5-to-cancel
            // poll path; handle_action would dispatch its own actions which
            // is not what we want during a long activity.
            handle_key_blocking_activity();
        }
        const auto t_after_input = std::chrono::steady_clock::now();
        const int waitev_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>( t_after_wait - t_iter0 ).count() );
        const int drain_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>( t_after_drain - t_after_wait ).count() );
        const int pump_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>( t_after_pump - t_after_drain ).count() );
        const int redraw_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>( t_after_redraw - t_after_pump ).count() );
        const int input_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>( t_after_input - t_after_redraw ).count() );
        max_waitev_ms = std::max( max_waitev_ms, waitev_ms );
        max_drain_ms  = std::max( max_drain_ms,  drain_ms );
        max_pump_ms   = std::max( max_pump_ms,   pump_ms );
        max_redraw_ms = std::max( max_redraw_ms, redraw_ms );
        max_input_ms  = std::max( max_input_ms,  input_ms );
        // Flag any single iter where a phase spent > 30ms — that's a smoking
        // gun for input lag, since the SDL queue can only drain via pump_events
        // and any phase that hogs the main thread blocks the next keypress.
        if( waitev_ms > 30 || drain_ms > 30 || pump_ms > 30 ||
            redraw_ms > 30 || input_ms > 30 ) {
            mp_log( "[cdda-mp] SRV-WAIT-ITER#" + std::to_string( iter_count ) +
                    " SLOW: wait=" + std::to_string( waitev_ms ) +
                    "ms drain=" + std::to_string( drain_ms ) +
                    "ms pump=" + std::to_string( pump_ms ) +
                    "ms redraw=" + std::to_string( redraw_ms ) +
                    "ms input=" + std::to_string( input_ms ) + "ms" );
        }
        iter_count++;
    }
    g_host_waiting_for_client = false;
    // Force a main-UI repaint each turn cycle.  During a long activity (|-wait,
    // crafting, sleep) the host's main game loop stays in a tight do_turn() loop
    // because the avatar always has moves; without this, the time/wait-% HUD
    // and recent messages don't visibly tick between turns.  ui_manager::redraw()
    // is a no-op unless something has been invalidated.
    g->invalidate_main_ui_adaptor();
    ui_manager::redraw();
    g_wait_elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start ).count() );
    {
        const player_activity &ha = get_avatar().activity;
        mp_log( "[cdda-mp] SRV-WAIT: done, elapsed=" + std::to_string( g_wait_elapsed_ms ) +
                "ms host_act=" + ( ha ? ha.id().str() : "none" ) +
                " host_in_wait=" + std::to_string( host_in_wait ) +
                " iters=" + std::to_string( iter_count ) +
                " acted_flag=" + std::to_string( g_client_acted_this_turn ) +
                " max_wait=" + std::to_string( max_waitev_ms ) +
                "ms max_drain=" + std::to_string( max_drain_ms ) +
                "ms max_pump=" + std::to_string( max_pump_ms ) +
                "ms max_redraw=" + std::to_string( max_redraw_ms ) +
                "ms max_input=" + std::to_string( max_input_ms ) + "ms" );
    }

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

bool host_is_in_wait_activity()
{
    if( !is_hosting() ) {
        return false;
    }
    static const activity_id act_wait( "ACT_WAIT" );
    static const activity_id act_wait_stamina( "ACT_WAIT_STAMINA" );
    static const activity_id act_wait_weather( "ACT_WAIT_WEATHER" );
    static const activity_id act_wait_npc( "ACT_WAIT_NPC" );
    const player_activity &act = get_avatar().activity;
    return act && ( act.id() == act_wait || act.id() == act_wait_stamina ||
                    act.id() == act_wait_weather || act.id() == act_wait_npc );
}

bool should_advance_calendar()
{
    // SP / host: always advance.  Client: only advance when moves > 0 — that
    // means a grant just landed and the current do_turn iteration represents
    // an actual game turn.  Without this guard, calendar::turn would tick at
    // every main-loop iteration (~10/sec) regardless of whether the client
    // had been granted a turn, racing past the host's authoritative time.
    return !is_client_mode() || get_avatar().get_moves() > 0;
}

bool is_passive_activity( const std::string &activity_id_str )
{
    // Activities where the avatar is committed turn-after-turn without
    // per-turn user input.  Once entered, SP's activity_actor::do_turn ticks
    // the activity on every game turn at machine speed.  Excludes:
    //  - ACT_FIRSTAID, ACT_AIM, ACT_AUTOATTACK, ACT_AUTODRIVE — interactive
    //  - ACT_NULL / empty — not in an activity
    static const std::set<std::string> passive = {
        "ACT_CRAFT", "ACT_LONGCRAFT", "ACT_DISASSEMBLE", "ACT_DISMEMBER",
        "ACT_READ",
        "ACT_EAT", "ACT_DRINK", "ACT_CONSUME", "ACT_CONSUME_DRINK_MENU",
        "ACT_CONSUME_FOOD_MENU", "ACT_CONSUME_MEDS_MENU",
        "ACT_BUTCHER", "ACT_BUTCHER_FULL", "ACT_FIELD_DRESS",
        "ACT_SKIN", "ACT_DISSECT", "ACT_QUARTER",
        "ACT_CONSTRUCTION", "ACT_BUILD",
        "ACT_VEHICLE",
        "ACT_WORKOUT_LIGHT", "ACT_WORKOUT_MODERATE", "ACT_WORKOUT_ACTIVE",
        "ACT_WORKOUT_HARD", "ACT_WORKOUT",
        "ACT_FORAGE", "ACT_FISH",
        "ACT_FILL_LIQUID", "ACT_PICKUP", "ACT_MOVE_ITEMS",
        "ACT_WAIT", "ACT_WAIT_STAMINA", "ACT_WAIT_WEATHER", "ACT_WAIT_NPC",
        "ACT_SLEEP",
        "ACT_HELP_PARTNER",
    };
    return passive.count( activity_id_str ) > 0;
}

bool should_fast_forward()
{
    // Need to be in an MP session.  SP never fast-forwards — SP runs activities
    // at machine speed already.
    if( !is_hosting() && !is_client_mode() ) {
        return false;
    }
    // Local avatar must be in a passive activity.
    const player_activity &av_act = get_avatar().activity;
    if( !av_act || !is_passive_activity( av_act.id().str() ) ) {
        return false;
    }
    // Partner's reported activity must also be passive.  g_partner_activity is
    // set by the heartbeat / per-action enrich on the other side — empty when
    // partner is idle (input loop) which means strict lockstep applies.
    if( g_partner_activity.empty() || !is_passive_activity( g_partner_activity ) ) {
        return false;
    }
    // No explicit combat-mode gate here: SP's activity_actor::do_turn already
    // cancels the activity on hostile-in-sight (and on low HP, hunger, sound,
    // etc.).  When that fires, av_act becomes null, this returns false on the
    // next call, and the next do_turn returns to strict lockstep naturally.
    return true;
}

void set_last_monmove_ms( int ms )
{
    g_last_monmove_ms = ms;
}

void set_last_host_action_label( const std::string &label )
{
    // Normalize directional action_ident() output ("RIGHT", "LEFTUP", etc.)
    // into the client's "move:DIR" format so the HUD's "Queued" row reads
    // identically on both ends.  Non-movement labels pass through unchanged.
    static const std::unordered_map<std::string, std::string> dir_map = {
        { "UP", "move:n" },  { "DOWN", "move:s" },
        { "LEFT", "move:w" }, { "RIGHT", "move:e" },
        { "LEFTUP", "move:nw" }, { "RIGHTUP", "move:ne" },
        { "LEFTDOWN", "move:sw" }, { "RIGHTDOWN", "move:se" },
        { "LEVEL_UP", "move:up" }, { "LEVEL_DOWN", "move:down" },
    };
    const auto it = dir_map.find( label );
    g_last_host_action_label = ( it != dir_map.end() ) ? it->second : label;
}

bool is_remote_player( character_id id )
{
    return remote_player_connected && id == remote_player_npc_id;
}

bool is_host_waiting_for_client()
{
    return g_host_waiting_for_client;
}

bool client_acted_this_turn()
{
    return g_client_acted_this_turn;
}

bool is_partner_in_wait_activity()
{
    // g_partner_activity is the activity id string last broadcast from the
    // other side (host_activity field for the client; client_activity field
    // for the host).  Only the four wait variants count as interruptible.
    return g_partner_activity == "ACT_WAIT" ||
           g_partner_activity == "ACT_WAIT_STAMINA" ||
           g_partner_activity == "ACT_WAIT_WEATHER" ||
           g_partner_activity == "ACT_WAIT_NPC";
}

bool is_partner_helping_us()
{
    return g_partner_activity == "ACT_HELP_PARTNER";
}

bool partner_activity_accepts_help()
{
    // SP's helper system (get_crafting_helpers + skill/proficiency math)
    // engages for these activities.  ACT_READ is intentionally excluded:
    // its "learn alongside" semantics need a separate design pass.
    static const std::set<std::string> eligible = {
        "ACT_CRAFT",
        "ACT_LONG_CRAFT",
        "ACT_BUILD",
        "ACT_VEHICLE",
        "ACT_VEHICLE_REPAIR",
        "ACT_BUTCHER",
        "ACT_BUTCHER_FULL",
        "ACT_FIELD_DRESS",
        "ACT_QUARTER",
        "ACT_DISMEMBER",
        "ACT_SKIN",
        "ACT_DISASSEMBLE",
        "ACT_DISASSEMBLE_RECURSIVELY",
    };
    return eligible.count( g_partner_activity ) > 0;
}

int partner_activity_moves_total()
{
    return g_partner_activity_moves_total;
}

int partner_activity_pct()
{
    return g_partner_activity_pct;
}

bool mp_in_burst_mode()
{
    // Both sides committed to non-interactive activities (neither needs
    // user input this turn).  Skip the lockstep throttle so the calendar
    // doesn't crawl at 1 turn/sec when nobody is actually playing.
    if( !is_hosting() && !is_client_mode() ) {
        return false;
    }
    if( !get_avatar().activity ) {
        return false;
    }
    if( g_partner_activity.empty() ) {
        return false;
    }
    return true;
}

// Compute a 0–100 progress percent for an arbitrary player_activity.  Most
// actors populate moves_total + moves_left, but a few (craft, vehicle) leave
// moves_total at 0 and track progress elsewhere.  This helper hides those
// special cases so the wire field `*_activity_pct` reflects what the player
// sees in their wait popup, not 0%.
int mp_compute_activity_pct( const player_activity &act )
{
    if( !act ) {
        return 0;
    }
    // Helper activity: mirror the partner's reported progress so both sides
    // display the same percent.  ACT_HELP_PARTNER uses a long fallback
    // duration on assign (since craft/vehicle leave moves_total=0), which
    // would otherwise read 1% throughout the entire help.  Check this BEFORE
    // the moves_total branch.
    static const activity_id ACT_HELP_PARTNER_ID( "ACT_HELP_PARTNER" );
    if( act.id() == ACT_HELP_PARTNER_ID ) {
        return g_partner_activity_pct;
    }
    // Standard path — works for ACT_WAIT and any other actor that sets
    // moves_total properly.
    if( act.moves_total > 0 ) {
        const int done = act.moves_total - act.moves_left;
        return std::clamp( done * 100 / act.moves_total, 0, 100 );
    }
    // Crafting: progress lives in the craft item's item_counter (scale of
    // 100,000 per percent, max 10,000,000).  Same calculation as SP's
    // tname display (`"%s (%d%%)"`).
    static const activity_id ACT_CRAFT_ID( "ACT_CRAFT" );
    static const activity_id ACT_LONG_CRAFT_ID( "ACT_LONG_CRAFT" );
    if( act.id() == ACT_CRAFT_ID || act.id() == ACT_LONG_CRAFT_ID ) {
        if( !act.targets.empty() && act.targets[0] ) {
            const item *it = act.targets[0].get_item();
            if( it && it->is_craft() ) {
                return std::clamp( it->item_counter / 100000, 0, 100 );
            }
        }
    }
    return 0;
}

// If our avatar is locally running ACT_HELP_PARTNER but the partner just
// finished / cancelled / switched to a non-helper-eligible activity, cancel
// our help commitment so we get our moves back and the SP helper bonus stops
// applying on their side.  Called from each wire-parse site that mutates
// g_partner_activity.  Safe to call when the avatar isn't helping (no-op).
static void mp_cancel_help_if_partner_done()
{
    avatar &av = get_avatar();
    if( !av.activity || av.activity.id().str() != "ACT_HELP_PARTNER" ) {
        return;
    }
    if( partner_activity_accepts_help() ) {
        return;
    }
    mp_log( "[cdda-mp] HELP-CANCEL: partner activity ended, dropping ACT_HELP_PARTNER" );
    add_msg( m_info, _( "Your partner finished — you stop helping." ) );
    av.cancel_activity();
}

void mark_wake_client_pending()
{
    g_pending_wake_client = true;
}

int get_separation_tier()
{
    return g_separation_tier;
}

npc *get_partner_npc()
{
    // Host POV: client's proxy. Client POV: host's proxy. Don't use
    // character_id::is_valid() — it rejects values <= 0, but MP-spawned
    // proxy NPCs end up with negative IDs. Check the raw value instead.
    if( remote_player_npc_id.get_value() != 0 ) {
        if( npc *n = g->critter_by_id<npc>( remote_player_npc_id ) ) {
            return n;
        }
    }
    if( client_host_npc_id.get_value() != 0 ) {
        if( npc *n = g->critter_by_id<npc>( client_host_npc_id ) ) {
            return n;
        }
    }
    return nullptr;
}

bool is_partner_npc( character_id id )
{
    // Host side: the client's proxy is remote_player_npc_id.
    if( is_remote_player( id ) ) {
        return true;
    }
    // Client side: the host's proxy is client_host_npc_id.  Don't use
    // character_id::is_valid() — it checks value > 0, but the MP-spawned
    // proxy NPCs end up with negative IDs (e.g. -1) which is_valid() rejects
    // even though the NPC is real and findable.  Just guard against the
    // default-uninitialized id (value 0) and compare directly.
    return is_client_mode() &&
           client_host_npc_id.get_value() != 0 &&
           id == client_host_npc_id;
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

void mp_notify_session_ending()
{
    if( is_host_mode() ) {
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( "{\"type\":\"session_ending\",\"from\":\"host\"}\n" );
            mp_log( "[cdda-mp] SESSION-END: host notified client" );
        }
    } else if( is_client_mode() ) {
        // Wrapped as an action so handle_remote_action()'s dispatcher sees it.
        client_send( "{\"type\":\"action\",\"action\":\"session_ending\",\"from\":\"client\"}" );
        mp_log( "[cdda-mp] SESSION-END: client notified host" );
    } else {
        return;
    }
    // Brief pause so the TCP write completes before the socket goes down.
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
}

// Enumerate `*.template` basenames in the user templates dir.
static std::vector<std::string> mp_local_template_names()
{
    std::vector<std::string> out;
    for( std::string p : get_files_from_path( ".template", PATH_INFO::templatedir(),
            false, true ) ) {
        p.erase( p.find( ".template" ), std::string::npos );
        p.erase( 0, p.find_last_of( "\\/" ) + 1 );
        out.push_back( p );
    }
    return out;
}

// Pick the right transport for outgoing MP messages from this side.  Both
// templates messages and the join-time list go through here so we don't keep
// branching on host/client at every send site.
static void mp_send_payload( const std::string &payload )
{
    if( is_host_mode() ) {
        server *srv = get_active_server();
        if( srv ) {
            srv->post_broadcast( payload + "\n" );
        }
    } else if( is_client_mode() ) {
        client_send( payload );
    }
}

void mp_templates_sync_on_join()
{
    const std::vector<std::string> names = mp_local_template_names();
    std::ostringstream oss;
    {
        JsonOut jo( oss );
        jo.start_object();
        jo.member( "type", "templates_list" );
        jo.member( "names", names );
        jo.end_object();
    }
    mp_send_payload( oss.str() );
    mp_log( "[cdda-mp] TEMPLATES: sent list, n=" + std::to_string( names.size() ) );
}

static void mp_handle_templates_list( const std::string &msg )
{
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        if( !jo.has_array( "names" ) ) {
            return;
        }
        std::set<std::string> local;
        for( const std::string &n : mp_local_template_names() ) {
            local.insert( n );
        }
        std::vector<std::string> missing;
        for( const JsonValue &v : jo.get_array( "names" ) ) {
            const std::string n = v.get_string();
            if( local.find( n ) == local.end() ) {
                missing.push_back( n );
            }
        }
        if( missing.empty() ) {
            mp_log( "[cdda-mp] TEMPLATES: nothing to request from partner" );
            return;
        }
        std::ostringstream oss;
        {
            JsonOut out( oss );
            out.start_object();
            out.member( "type", "template_request" );
            out.member( "names", missing );
            out.end_object();
        }
        mp_send_payload( oss.str() );
        mp_log( "[cdda-mp] TEMPLATES: requested " + std::to_string( missing.size() ) );
    } catch( const JsonError &e ) {
        mp_log( "[cdda-mp] TEMPLATES list parse error: " + std::string( e.what() ) );
    }
}

static void mp_handle_template_request( const std::string &msg )
{
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        if( !jo.has_array( "names" ) ) {
            return;
        }
        int sent = 0;
        for( const JsonValue &v : jo.get_array( "names" ) ) {
            const std::string name = v.get_string();
            const std::string path = PATH_INFO::templatedir() + name + ".template";
            std::optional<std::string> content = read_whole_file( path );
            if( !content ) {
                mp_log( "[cdda-mp] TEMPLATES: requested template missing locally: " + name );
                continue;
            }
            std::ostringstream oss;
            {
                JsonOut out( oss );
                out.start_object();
                out.member( "type", "template_data" );
                out.member( "name", name );
                out.member( "content", *content );
                out.end_object();
            }
            mp_send_payload( oss.str() );
            ++sent;
        }
        mp_log( "[cdda-mp] TEMPLATES: sent " + std::to_string( sent ) + " requested" );
    } catch( const JsonError &e ) {
        mp_log( "[cdda-mp] TEMPLATES request parse error: " + std::string( e.what() ) );
    }
}

static void mp_handle_template_data( const std::string &msg )
{
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        const std::string name = jo.get_string( "name", "" );
        const std::string content = jo.get_string( "content", "" );
        if( name.empty() || content.empty() ) {
            return;
        }
        const std::string path = PATH_INFO::templatedir() + name + ".template";
        // Local-wins on name collision so a player can't have their custom
        // template silently replaced by a partner's same-named one.
        if( file_exist( path ) ) {
            mp_log( "[cdda-mp] TEMPLATES: skip overwrite of existing: " + name );
            return;
        }
        const bool ok = write_to_file( path, [&]( std::ostream & out ) {
            out << content;
        }, _( "received template" ) );
        if( ok ) {
            mp_log( "[cdda-mp] TEMPLATES: wrote received '" + name + "'" );
        }
    } catch( const JsonError &e ) {
        mp_log( "[cdda-mp] TEMPLATES data parse error: " + std::string( e.what() ) );
    }
}

// Set when the user picks "Host a session" from the main-menu chooser.
// The server thread isn't actually spawned yet — that's deferred to the
// first process_mp_events() call (which only runs once a world is loaded),
// so a client connecting before there's an avatar to spawn into can't crash
// us.  Lets us also offer a "Cancel co-op" path while still in the menu.
static bool g_pending_host_start = false;
static bool g_host_thread_actually_started = false;

// Called from process_mp_events() on the host's first turn after the world
// has loaded.  Spawns the listen-server thread iff the menu armed it and we
// haven't already started it.  No-op when the server was started via the
// --host CLI flag (main.cpp spawns its own thread in that path).
void mp_start_pending_host_thread()
{
    if( !g_pending_host_start || g_host_thread_actually_started ) {
        return;
    }
    std::thread( []() {
        run_server( 8080, std::string() );
    } ).detach();
    g_host_thread_actually_started = true;
    mp_log( "[cdda-mp] MENU: host thread started (post-world-load)" );
}

bool mp_menu_start_host_session()
{
    if( is_host_mode() ) {
        // Already armed (or thread already running) — treat as success so the
        // caller can re-enter the world / char-creation flow.
        return true;
    }
    set_host_mode( true );
    // Server thread starts on the host's first do_turn (see
    // mp_start_pending_host_thread) so we don't end up listening before
    // there's a world for incoming clients to spawn into.
    g_pending_host_start = true;
    g_host_thread_actually_started = false;
    mp_log( "[cdda-mp] MENU: host armed on port 8080 (thread deferred to do_turn)" );
    return true;
}

void mp_menu_cancel_host()
{
    if( !g_pending_host_start && !is_host_mode() ) {
        return;
    }
    g_pending_host_start = false;
    set_host_mode( false );
    mp_log( "[cdda-mp] MENU: host-mode cancelled from co-op chooser" );
}

WORLD *mp_ensure_client_scratch_world()
{
    static const std::string SCRATCH_NAME = "_coop_client_scratch";
    if( WORLD *w = world_generator->get_world( SCRATCH_NAME ) ) {
        return w;
    }
    const std::vector<mod_id> default_mods = { mod_id( "dda" ) };
    WORLD *neww = world_generator->make_new_world( SCRATCH_NAME, default_mods );
    if( neww ) {
        mp_log( "[cdda-mp] MENU: created client scratch world '" + SCRATCH_NAME + "'" );
    } else {
        mp_log( "[cdda-mp] MENU: failed to create client scratch world" );
    }
    return neww;
}

// Returns the path to a world's mp_world.json sidecar.  Empty when the
// named world doesn't exist (e.g. picker is looking at a stale name).
static cata_path mp_world_marker_path( const std::string &worldname )
{
    WORLD *w = world_generator->get_world( worldname );
    if( !w ) {
        return cata_path();
    }
    return w->folder_path() / "mp_world.json";
}

mp_world_marker mp_world_marker_load( const std::string &worldname )
{
    mp_world_marker m;
    const cata_path path = mp_world_marker_path( worldname );
    if( path.get_unrelative_path().empty() ) {
        return m;
    }
    read_from_file_optional_json( path, [&]( const JsonValue & jv ) {
        JsonObject jo = jv.get_object();
        m.first_seen_iso = jo.get_string( "first_seen_iso", std::string() );
        m.last_seen_iso  = jo.get_string( "last_seen_iso",  std::string() );
        m.last_role      = jo.get_string( "last_role",      std::string() );
    } );
    return m;
}

static void mp_world_marker_save( const std::string &worldname,
                                  const mp_world_marker &m )
{
    const cata_path path = mp_world_marker_path( worldname );
    if( path.get_unrelative_path().empty() ) {
        return;
    }
    write_to_file( path, [&]( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_object();
        jo.member( "first_seen_iso", m.first_seen_iso );
        jo.member( "last_seen_iso",  m.last_seen_iso );
        jo.member( "last_role",      m.last_role );
        jo.end_object();
    }, "mp world marker" );
}

bool mp_world_has_history( const std::string &worldname )
{
    WORLD *w = world_generator->get_world( worldname );
    if( !w ) {
        return false;
    }
    const cata_path folder = w->folder_path();
    // Primary marker.
    if( file_exist( folder / "mp_world.json" ) ) {
        return true;
    }
    // Fallback for worlds that pre-date the marker: any mp_player_*.json
    // or mp_npc_cleanup.json sitting in the world folder is a tell.
    if( file_exist( folder / "mp_npc_cleanup.json" ) ) {
        return true;
    }
    const std::vector<cata_path> players = get_files_from_path(
            "mp_player_", folder, false, false );
    return !players.empty();
}

std::string mp_world_marker_badge( const std::string &worldname )
{
    if( !mp_world_has_history( worldname ) ) {
        return std::string();
    }
    const mp_world_marker m = mp_world_marker_load( worldname );
    std::string body = "co-op";
    if( !m.last_role.empty() ) {
        body += ", " + m.last_role;
    }
    return "  (" + body + ")";
}

bool mp_load_promote_prompt( const std::string &worldname )
{
    if( !mp_world_has_history( worldname ) ) {
        return true;
    }
    // If already armed/connected from a previous menu interaction, no point
    // re-prompting — just continue.
    if( is_host_mode() || is_client_mode() ) {
        return true;
    }
    uilist menu;
    menu.title = _( "This world has co-op history" );
    menu.text = _( "Load as solo, or arm Host so your partner can Join?" );
    // Distinct positive retvals + an explicit cancel sentinel.  Avoids any
    // ambiguity with uilist's own UILIST_CANCEL (-1027) on ESC.
    constexpr int RET_SOLO = 1;
    constexpr int RET_HOST = 2;
    constexpr int RET_CANCEL = 3;
    menu.entries.emplace_back( RET_SOLO,   true, 's', _( "Load solo" ) );
    menu.entries.emplace_back( RET_HOST,   true, 'h', _( "Arm Host (co-op)" ) );
    menu.entries.emplace_back( RET_CANCEL, true, 'q', _( "Cancel" ) );
    menu.query();
    if( menu.ret == RET_SOLO ) {
        mp_log( "[cdda-mp] promote-prompt: solo for " + worldname );
        return true;
    }
    if( menu.ret == RET_HOST ) {
        mp_log( "[cdda-mp] promote-prompt: arm host for " + worldname );
        mp_menu_start_host_session();
        return true;
    }
    mp_log( "[cdda-mp] promote-prompt: cancel for " + worldname + " (ret=" + std::to_string(
                menu.ret ) + ")" );
    return false;
}

void mp_world_marker_update()
{
    if( !is_host_mode() && !is_client_mode() ) {
        return;
    }
    if( !world_generator || !world_generator->active_world ) {
        return;
    }
    const std::string &worldname = world_generator->active_world->world_name;
    if( worldname.empty() ) {
        return;
    }
    // One write per (process, world).  Resets when the loaded world changes.
    static std::string last_marked_world;
    if( last_marked_world == worldname ) {
        return;
    }
    last_marked_world = worldname;

    // Local-time string is fine; mp_world.json is human-readable, not parsed
    // for arithmetic.
    const std::time_t now = std::time( nullptr );
    char buf[32] = {0};
    std::strftime( buf, sizeof( buf ), "%Y-%m-%d %H:%M:%S",
                   std::localtime( &now ) );
    const std::string now_iso( buf );

    mp_world_marker m = mp_world_marker_load( worldname );
    if( m.first_seen_iso.empty() ) {
        m.first_seen_iso = now_iso;
    }
    m.last_seen_iso = now_iso;
    m.last_role = is_host_mode() ? "host" : "client";
    mp_world_marker_save( worldname, m );
    mp_log( "[cdda-mp] world-marker: wrote " + worldname + " as " + m.last_role );
}

static cata_path mp_recent_hosts_path()
{
    return PATH_INFO::config_dir_path() / "mp_recent_hosts.json";
}

std::vector<mp_recent_host> mp_recent_hosts_load()
{
    std::vector<mp_recent_host> out;
    read_from_file_optional_json( mp_recent_hosts_path(), [&]( const JsonValue & jv ) {
        JsonObject jo = jv.get_object();
        if( !jo.has_array( "hosts" ) ) {
            return;
        }
        for( JsonObject e : jo.get_array( "hosts" ) ) {
            mp_recent_host rh;
            rh.host = e.get_string( "host", std::string() );
            rh.port = static_cast<uint16_t>( e.get_int( "port", 8080 ) );
            rh.label = e.get_string( "label", std::string() );
            if( !rh.host.empty() ) {
                out.push_back( std::move( rh ) );
            }
        }
    } );
    return out;
}

void mp_recent_hosts_save( const std::vector<mp_recent_host> &hosts )
{
    write_to_file( mp_recent_hosts_path(), [&]( std::ostream & fout ) {
        JsonOut jo( fout );
        jo.start_object();
        jo.member( "hosts" );
        jo.start_array();
        for( const mp_recent_host &h : hosts ) {
            jo.start_object();
            jo.member( "host", h.host );
            jo.member( "port", static_cast<int>( h.port ) );
            jo.member( "label", h.label );
            jo.end_object();
        }
        jo.end_array();
        jo.end_object();
    }, "mp recent hosts" );
}

void mp_recent_hosts_remember( const std::string &host, uint16_t port,
                               const std::string &label )
{
    constexpr size_t MP_RECENT_HOSTS_CAP = 8;
    std::vector<mp_recent_host> list = mp_recent_hosts_load();
    std::string preserved_label = label;
    auto same = [&]( const mp_recent_host & h ) {
        return h.host == host && h.port == port;
    };
    auto it = std::find_if( list.begin(), list.end(), same );
    if( it != list.end() ) {
        if( preserved_label.empty() ) {
            preserved_label = it->label;
        }
        list.erase( it );
    }
    list.insert( list.begin(), mp_recent_host{ host, port, preserved_label } );
    if( list.size() > MP_RECENT_HOSTS_CAP ) {
        list.resize( MP_RECENT_HOSTS_CAP );
    }
    mp_recent_hosts_save( list );
}

// Parse "host[:port]" into separate host + port.  Returns false (and pops an
// error) on a malformed port; default port is 8080.
static bool mp_parse_address( const std::string &entered, std::string &host,
                              uint16_t &port )
{
    host = entered;
    port = 8080;
    const size_t colon = entered.rfind( ':' );
    if( colon != std::string::npos ) {
        host = entered.substr( 0, colon );
        try {
            port = static_cast<uint16_t>( std::stoi( entered.substr( colon + 1 ) ) );
        } catch( const std::exception & ) {
            popup( _( "Invalid port in '%s'." ), entered.c_str() );
            return false;
        }
    }
    return true;
}

bool mp_menu_join_session()
{
    if( is_client_mode() ) {
        // Already connected — treat as success so caller can drive the
        // next UI step (char-creation flow).
        return true;
    }

    std::vector<mp_recent_host> recent = mp_recent_hosts_load();
    std::string entered;
    std::string label_in;       // existing label if user picked from history
    if( !recent.empty() ) {
        uilist menu;
        menu.title = _( "Co-op: join a session" );
        int idx = 0;
        for( const mp_recent_host &rh : recent ) {
            std::string display = rh.host + ":" + std::to_string( rh.port );
            if( !rh.label.empty() ) {
                display = rh.label + "  —  " + display;
            }
            menu.entries.emplace_back( idx++, true, MENU_AUTOASSIGN, display );
        }
        const int new_addr_idx = idx;
        menu.entries.emplace_back( new_addr_idx, true, 'n', _( "Enter new address…" ) );
        menu.entries.emplace_back( -1, true, 'q', _( "Cancel" ) );
        menu.query();
        if( menu.ret < 0 ) {
            return false;
        }
        if( menu.ret < static_cast<int>( recent.size() ) ) {
            const mp_recent_host &rh = recent[menu.ret];
            entered = rh.host + ":" + std::to_string( rh.port );
            label_in = rh.label;
        }
        // else: "Enter new address" — fall through to the input popup
    }
    if( entered.empty() ) {
        entered = string_input_popup()
                  .title( _( "Host address  (e.g. 192.168.1.5  or  100.64.0.5:8080)" ) )
                  .width( 40 )
                  .query_string();
        if( entered.empty() ) {
            return false;
        }
    }

    std::string host;
    uint16_t port = 8080;
    if( !mp_parse_address( entered, host, port ) ) {
        return false;
    }
    // Pre-flight TCP probe so a typo'd IP returns in ~3 s instead of hanging
    // on macOS's 75 s default SYN retry.  Only on success do we commit to
    // setting client_mode and running the real connect handshake.
    if( !tcp_probe( host, port, 3000 ) ) {
        popup( _( "Could not reach %s:%d.\n\nCheck the address, the host is running, and the port (default 8080) isn't blocked." ),
               host.c_str(), static_cast<int>( port ) );
        return false;
    }
    set_client_mode( true );
    if( !client_connect( host, port, "player2", std::string() ) ) {
        popup( _( "Could not connect to %s:%d." ), host.c_str(), static_cast<int>( port ) );
        set_client_mode( false );
        return false;
    }
    mp_log( "[cdda-mp] MENU: client connected to " + host + ":" + std::to_string( port ) );

    // Only ask for a label the first time we see this address.  Skipping is
    // fine — the address alone is also a useful identifier.
    std::string label_out = label_in;
    if( label_in.empty() ) {
        label_out = string_input_popup()
                    .title( _( "Save as (optional label, e.g. 'Intel Mac' — blank to skip)" ) )
                    .width( 30 )
                    .query_string();
    }
    mp_recent_hosts_remember( host, port, label_out );
    return true;
}

std::string mp_menu_coop_status_text()
{
    if( is_host_mode() ) {
        if( g_pending_host_start && !g_host_thread_actually_started ) {
            return std::string( _( "Co-op: armed — re-enter Host to pick world / character" ) );
        }
        return std::string( _( "Co-op: hosting on port 8080 — waiting for partner" ) );
    }
    if( is_client_mode() ) {
        return std::string( _( "Co-op: connected to host — re-enter Join to pick character" ) );
    }
    return std::string();
}

// Check separation between two absolute positions and update g_separation_tier.
// Uses Chebyshev distance (same as rl_dist in 2D).  Tier thresholds, sized
// for the MAPSIZE=15 bubble (~84-tile radius).  Tier 3 is the auto-pause
// threshold; tier 2 is an 8-tile leeway zone above the warning so a driver
// at highway speed gets braking room before time freezes.
//   0 → 1 at ≥40 tiles  (mild warning)
//   1 → 2 at ≥60 tiles  (urgent — brake/turn around, 8 tiles of leeway)
//   2 → 3 at ≥68 tiles  (pause world clock until separation drops back)
//   3 → 2 at <63 tiles, 2 → 1 at <55, 1 → 0 at <34   (hysteresis)
static void check_separation_warning( const tripoint_abs_ms &a, const tripoint_abs_ms &b )
{
    const int dist = std::max( std::abs( a.x() - b.x() ), std::abs( a.y() - b.y() ) );
    const int prev = g_separation_tier;
    if( dist >= 68 ) {
        g_separation_tier = 3;
    } else if( dist >= 60 ) {
        g_separation_tier = std::max( g_separation_tier, 2 );
    } else if( dist >= 40 ) {
        g_separation_tier = std::max( std::min( g_separation_tier, 2 ), 1 );
    } else if( dist < 34 ) {
        g_separation_tier = 0;
    } else if( dist < 55 ) {
        g_separation_tier = std::min( g_separation_tier, 1 );
    } else if( dist < 63 ) {
        g_separation_tier = std::min( g_separation_tier, 2 );
    }
    if( g_separation_tier != prev ) {
        if( g_separation_tier == 0 ) {
            add_msg( m_good, "You and your partner are close enough again." );
        } else if( g_separation_tier == 1 ) {
            add_msg( m_warning, "Your partner is getting far away (%d tiles). Max safe range is ~80.", dist );
        } else if( g_separation_tier == 2 ) {
            add_msg( m_bad, "Your partner is near the edge of the simulated zone (%d tiles)! Brake or turn around.", dist );
        } else {
            add_msg( m_bad, "Your partner is past the edge (%d tiles)! Vehicle physics will break — close the gap now.", dist );
        }
    }
}

void process_mp_events()
{
    // If the menu armed host mode but the world wasn't loaded yet, the
    // listen-server thread was deferred until now.  Starts on the first
    // do_turn() after the avatar exists so a client connecting can actually
    // be spawned into a world.  No-op for --host CLI launches.
    mp_start_pending_host_thread();

    // On the very first tick, purge any MP NPCs that leaked into the world save
    // from a previous session (server and client share the same world directory).
    mp_cleanup_stale_npcs();

    // Stamp this world as MP-touched so the world pickers can badge it.
    mp_world_marker_update();

    mp_event event;
    while( get_mp_queue().pop( event ) ) {
        if( event.evt_type == mp_event::type::action ) {
            mp_log( "[cdda-mp] process_mp_events: " + event.data.substr( 0, 60 ) );
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

static void update_client_host_npc( const tripoint_abs_ms &abs_pos, const std::string &name,
                                    bool host_in_vehicle, bool host_ctrl_veh )
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
            // Mirror SP avatar_action::move boarding semantics for the host proxy:
            // unboard from current tile before setpos, board at the new tile if
            // boardable AND the host says they're in a vehicle.  Without this the
            // proxy renders walking next to the car while the host is driving.
            // Guard against stale in_vehicle (set by previous broadcast where the
            // host was in a vehicle that has since moved or unloaded).
            if( host_npc->in_vehicle ) {
                if( m.veh_at( host_npc->pos_bub() ).part_with_feature( "BOARDABLE", true ) ) {
                    m.unboard_vehicle( host_npc->pos_bub() );
                } else {
                    host_npc->in_vehicle = false;
                    host_npc->controlling_vehicle = false;
                }
            }
            host_npc->setpos( m, bub );
            if( host_in_vehicle &&
                m.veh_at( host_npc->pos_bub() ).part_with_feature( "BOARDABLE", true ) ) {
                m.board_vehicle( host_npc->pos_bub(), host_npc );
            }
        }
    }
    // Apply driving state every tick so the proxy's flags track the host even when
    // position didn't change (idling at the wheel, cruise mode).
    host_npc->in_vehicle = host_in_vehicle;
    host_npc->controlling_vehicle = host_ctrl_veh;
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

    // Per-tile vehicle motion packet (sent during host's vehmove between
    // game turns).  Slim payload — just position/face/turn_dir/vel.  Apply
    // through the same vehicle sync path the full state packet uses, then
    // force a render so the new tile shows immediately instead of waiting
    // for the next user keypress.
    if( msg.find( "\"type\":\"vehicle_step\"" ) != std::string::npos ) {
        try {
            JsonValue jv = json_loader::from_string( msg );
            JsonObject jo = jv.get_object();
            jo.allow_omitted_members();
            apply_vehicle_sync( jo );
            g->invalidate_main_ui_adaptor();
            ui_manager::redraw();
            refresh_display();
        } catch( const JsonError &e ) {
            mp_log( "[cdda-mp] vehicle_step parse error: " + std::string( e.what() ) );
        }
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

    // Graceful session-end notification from the host (v1 save handshake).
    // Host has saved their world; the client's local .sav isn't written by
    // the current architecture, so this is messaging-only on this side.
    // After this the server socket will close shortly — the existing
    // disconnect handler takes over from there.
    if( msg.find( "\"type\":\"session_ending\"" ) != std::string::npos ) {
        mp_log( "[cdda-mp] SESSION-END RECV: host is leaving" );
        add_msg( m_warning, _( "Your partner is leaving.  The session will end shortly." ) );
        return true;
    }

    // Templates wire-sync handlers (symmetric — same shape on host and client).
    if( msg.find( "\"type\":\"templates_list\"" ) != std::string::npos ) {
        mp_handle_templates_list( msg );
        return true;
    }
    if( msg.find( "\"type\":\"template_request\"" ) != std::string::npos ) {
        mp_handle_template_request( msg );
        return true;
    }
    if( msg.find( "\"type\":\"template_data\"" ) != std::string::npos ) {
        mp_handle_template_data( msg );
        return true;
    }

    // Our character died on the host (proxy NPC was killed by monsters there).
    // Mirror the SP death path locally: zero our HP and let the next do_turn
    // trigger the standard death-screen / game-over flow.  Without this the
    // client would just see a "Lost connection to server" 20 seconds later
    // when the silent host stops granting turns — confusing UX for what is
    // really "your character died".
    if( msg.find( "\"type\":\"you_died\"" ) != std::string::npos ) {
        add_msg( m_bad, "%s", _( "You died." ) );
        get_avatar().die( &get_map(), nullptr );
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
        if( !host_name.empty() ) {
            g_partner_name_cached = host_name;
        }

        if( jo.has_object( "host_pos" ) ) {
            std::cout << "[cdda-mp] updating host NPC..." << std::flush;
            JsonObject hpos = jo.get_object( "host_pos" );
            hpos.allow_omitted_members();
            const tripoint_abs_ms host_pos{
                hpos.get_int( "x" ), hpos.get_int( "y" ), hpos.get_int( "z" )
            };
            const bool host_in_veh  = jo.has_bool( "host_in_vehicle" )
                                      ? jo.get_bool( "host_in_vehicle" ) : false;
            const bool host_ctrl_v  = jo.has_bool( "host_ctrl_veh" )
                                      ? jo.get_bool( "host_ctrl_veh" ) : false;
            update_client_host_npc( host_pos, host_name, host_in_veh, host_ctrl_v );
            std::cout << " ok" << std::endl;
        }

        // Track the host's current activity for HUD + partner-notice display.
        if( jo.has_string( "host_activity" ) ) {
            g_partner_activity = jo.get_string( "host_activity" );
            mp_partner_activity_transition_check();
        }
        if( jo.has_int( "host_activity_pct" ) ) {
            g_partner_activity_pct = jo.get_int( "host_activity_pct" );
        }
        if( jo.has_int( "host_activity_moves_total" ) ) {
            g_partner_activity_moves_total = jo.get_int( "host_activity_moves_total" );
        }
        // Snapshot host's calendar BEFORE the local sync above overwrites it,
        // so the panel can show drift = local - last_received_partner.  Since
        // the client sets local = host on every state packet, drift here is
        // the gap between packets — useful sanity indicator.
        if( jo.has_int( "calendar_turn" ) ) {
            g_partner_calendar_turn = jo.get_int( "calendar_turn" );
        }

        // Host→client tap-on-shoulder: cancel local wait activity if the
        // host's bump menu invoked Tap. Only ACT_WAIT variants are
        // interruptible at this stage (sleep / crafting / reading are left
        // alone). The host already side-emitted the appropriate add_msg on
        // its side; we add the symmetric message here so the client sees
        // who tapped them.
        if( jo.has_bool( "wake_client" ) && jo.get_bool( "wake_client" ) ) {
            avatar &u = get_avatar();
            static const activity_id s_act_wait( "ACT_WAIT" );
            static const activity_id s_act_wait_stamina( "ACT_WAIT_STAMINA" );
            static const activity_id s_act_wait_weather( "ACT_WAIT_WEATHER" );
            static const activity_id s_act_wait_npc( "ACT_WAIT_NPC" );
            const activity_id cur = u.activity.id();
            const bool was_waiting = cur == s_act_wait || cur == s_act_wait_stamina ||
                                     cur == s_act_wait_weather || cur == s_act_wait_npc;
            mp_log( "[cdda-mp] CLI-WAKE: was_waiting=" + std::to_string( was_waiting ) +
                    " activity=" + cur.str() );
            if( was_waiting ) {
                u.cancel_activity();
                std::string host_name = _( "Your partner" );
                if( client_host_npc_id.is_valid() ) {
                    if( npc *hnpc = g->critter_by_id<npc>( client_host_npc_id ) ) {
                        host_name = hnpc->get_name();
                    }
                }
                add_msg( _( "%s taps you on the shoulder, snapping you out of your wait." ),
                         host_name );
            }
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

        // "free":true means this isn't a turn-ending action.  Two cases:
        //  - Wall bump or refused action (moves<=0 broadcast, free=true)
        //  - Partial-turn update like pldrive that consumed only some AP and
        //    left budget for more driving inputs (moves>0, free=true).
        // Clear the ack guard so the restored/partial moves value is accepted
        // by the seq-bypassed path further down.
        const bool is_partial_turn = jo.has_bool( "free" ) && jo.get_bool( "free" );
        if( is_partial_turn ) {
            g_client_waiting_for_ack = false;
        }

        const uint32_t grant_seq = jo.has_int( "grant_seq" )
                                   ? static_cast<uint32_t>( jo.get_int( "grant_seq" ) ) : 0;

        // Apply server-authoritative move budget.
        // Two guards prevent double-application:
        //  1. ack guard: we sent an action and are waiting for the server's moves=0 ack
        //  2. seq guard: grant_seq <= last seen means this is a TCP-buffered duplicate
        // Safety: force-clear ack guard after 5 s to recover from any stuck state.
        if( g_client_waiting_for_ack ) {
            using namespace std::chrono_literals;
            if( std::chrono::steady_clock::now() - g_ack_set_time > 5s ) {
                mp_log( "[cdda-mp] ack guard timed out — force-clearing" );
                g_client_waiting_for_ack = false;
            }
        }
        if( jo.has_member( "moves" ) ) {
            const int srv_moves = jo.get_int( "moves" );
            mp_log( "[cdda-mp] MOVES-DEBUG: srv_moves=" + std::to_string( srv_moves ) +
                    " is_partial=" + std::to_string( is_partial_turn ) +
                    " ack=" + std::to_string( g_client_waiting_for_ack ) +
                    " grant_seq=" + std::to_string( grant_seq ) +
                    " last_seq=" + std::to_string( g_client_last_grant_seq ) +
                    " av_act=" + ( get_avatar().activity ? get_avatar().activity.id().str() : "none" ) );
            // Refresh the "last heard from host" timestamp on any moves-bearing
            // state message (grants AND ack-clears).  The wedge-breaker uses
            // this to detect "host went silent" rather than "host hasn't sent
            // a fresh grant" — the old metric let ack-clear-only periods (host
            // is busy processing our previous actions) look like wedges.
            g_last_grant_time = std::chrono::steady_clock::now();
            if( is_partial_turn && srv_moves > 0 ) {
                // Partial-turn update from host (e.g., pldrive consumed some AP
                // but left budget).  Bypass the seq guard — this isn't a fresh
                // grant, the host hasn't advanced the calendar.  Just sync the
                // remaining AP so the client can issue more driving inputs.
                mp_log( "[cdda-mp] CLI-PARTIAL: moves=" + std::to_string( srv_moves ) );
                get_avatar().set_moves( srv_moves );
            } else if( srv_moves <= 0 ) {
                // ACK: server confirmed our action was received.  Always apply.
                mp_log( "[cdda-mp] CLI-ACK-CLEAR: moves=" + std::to_string( srv_moves ) +
                        " seq=" + std::to_string( grant_seq ) +
                        " ack_was=" + std::to_string( g_client_waiting_for_ack ) +
                        " last_seq=" + std::to_string( g_client_last_grant_seq ) +
                        " pending=" + ( g_pending_action.empty() ? "none" : g_pending_action.substr( 0, 40 ) ) );
                g_client_waiting_for_ack = false;
                get_avatar().set_moves( srv_moves );
            } else if( ( !g_client_waiting_for_ack || get_avatar().activity ) &&
                       ( grant_seq == 0 || grant_seq > g_client_last_grant_seq ) ) {
                // New grant: seq is fresh AND (no pending ack OR avatar is in an
                // activity).  The activity-override bypasses the ack guard so
                // long activities (drop, read, craft) don't stall when grants
                // arrive faster than the client can ack — the activity is the
                // pacing authority during a passive activity, not the
                // handshake.  Seq guard still prevents stale replays.
                if( grant_seq > 0 ) {
                    g_client_last_grant_seq = grant_seq;
                }
                const player_activity &ca = get_avatar().activity;
                mp_log( "[cdda-mp] CLI-GRANT: moves=" + std::to_string( srv_moves ) +
                        " seq=" + std::to_string( grant_seq ) +
                        " act=" + ( ca ? ca.id().str() : "none" ) +
                        " override_ack=" + std::to_string( ca && g_client_waiting_for_ack ) );
                // If we overrode the ack guard for an activity, clear it now so
                // the next dispatch isn't suppressed by stale state.
                if( ca && g_client_waiting_for_ack ) {
                    g_client_waiting_for_ack = false;
                }
                get_avatar().set_moves( srv_moves );
                g_last_grant_time = std::chrono::steady_clock::now();
                // If the client is in any long activity, ack the grant
                // immediately with a "wait" action.  Without this, multiple
                // host grants pile up in the same process_mp_events drain and
                // only one gets acked per do_turn, so the host races ahead via
                // SAFETY-TIMEOUT and calendars desync.  Setting the ack guard
                // here also causes subsequent grants in the same drain to take
                // the CLI-SKIP branch, providing proper backpressure so the
                // host advances at the client's pace.
                if( ca ) {
                    // Any active player_activity in MP is a long action: it needs
                    // one tick per host grant, and we must ack the grant before
                    // the next message in this drain (the host's ack-clear) zeroes
                    // our moves.  Covers `|` wait, crafting, reading, butchering,
                    // mining, construction, repair, etc. — every activity_actor.
                    const std::string pre_tick_id = ca.id().str();
                    const int pre_tick_moves = get_avatar().get_moves();
                    const int pre_tick_moves_left = ca.moves_left;
                    get_avatar().activity.do_turn( get_avatar() );
                    const int post_tick_moves = get_avatar().get_moves();
                    const player_activity &post_ca = get_avatar().activity;
                    const int post_tick_moves_left = post_ca ? post_ca.moves_left : 0;
                    // If the activity completed during this tick, emit the
                    // explicit end signal BEFORE the wait so the host clears
                    // its lockstep-bypass state and the wait closes the turn.
                    // do_turn's outer activity_just_ended detector misses this
                    // case because pre_activity_id is captured AFTER
                    // client_process_incoming runs.
                    if( !get_avatar().activity ) {
                        g_client_turn_activity.clear();
                        client_send_activity_end( pre_tick_id );
                    }
                    mp_log( "[cdda-mp] CLI-GRANT-ACT-ACK: id=" + pre_tick_id
                            + " moves " + std::to_string( pre_tick_moves ) + "->" + std::to_string( post_tick_moves )
                            + " moves_left " + std::to_string( pre_tick_moves_left ) + "->" + std::to_string( post_tick_moves_left )
                            + " ended=" + std::to_string( !get_avatar().activity )
                            + " grant_seq=" + std::to_string( grant_seq ) );
                    client_send( client_enrich_action(
                                     "{\"type\":\"action\",\"action\":\"wait\"}" ) );
                    g_client_waiting_for_ack = true;
                    g_ack_set_time = std::chrono::steady_clock::now();
                    // Mirror the host's per-turn redraw so the client's HUD
                    // (time, activity %, environmental messages) ticks live
                    // while the activity runs.  Without this, do_turn never
                    // exits to the main game loop's redraw path during a long
                    // activity.
                    g->invalidate_main_ui_adaptor();
                }
            } else {
                // Stale: ack pending or seq already seen.
                mp_log( "[cdda-mp] CLI-SKIP: moves=" + std::to_string( srv_moves ) +
                        " seq=" + std::to_string( grant_seq ) + "/" +
                        std::to_string( g_client_last_grant_seq ) +
                        " ack=" + std::to_string( g_client_waiting_for_ack ) +
                        " reason=" + ( g_client_waiting_for_ack ? "ack-pending" : "old-seq" ) );
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
            // Loop-break: messages forwarded FROM the host must not be picked up
            // by client_capture_avatar_msgs and forwarded back via client_msgs.
            // Otherwise the host's "You X" → client adds → client captures and
            // sends "Name X" back → host adds → host re-substitutes to "You X"
            // → forwards back forever.  Advancing the watermark past these
            // messages keeps them local-display-only.
            g_client_msg_watermark = Messages::size();
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
        // Just sent the join — clear any stale ack/seq state from a previous session
        // so the server's first move grant isn't silently ignored after reconnect.
        g_client_waiting_for_ack = false;
        g_client_last_grant_seq = 0;
        // Immediately follow with our worn-item list and skin tone.
        client_resync_worn();
        // Templates wire-sync: send local template list so the host can request
        // anything it's missing.  Host sends its own list independently.
        mp_templates_sync_on_join();
    }
    std::string msg;
    int recv_count = 0;
    while( client_recv_pop( msg ) ) {
        ++recv_count;
        const auto m_pos = msg.find( "\"moves\":" );
        const std::string moves_str = ( m_pos != std::string::npos )
                                      ? msg.substr( m_pos, 16 ) : "no-moves";
        const int pre_apply_moves = get_avatar().get_moves();
        mp_log( "[cdda-mp] CLI-RECV#" + std::to_string( recv_count ) + ": " + moves_str +
                " pre_apply_moves=" + std::to_string( pre_apply_moves ) );
        apply_one_state_message( msg );
        const int post_apply_moves = get_avatar().get_moves();
        if( post_apply_moves != pre_apply_moves ) {
            mp_log( "[cdda-mp] CLI-RECV#" + std::to_string( recv_count ) +
                    ": moves changed " + std::to_string( pre_apply_moves ) + "->" +
                    std::to_string( post_apply_moves ) +
                    " pending=" + ( g_pending_action.empty() ? "none" : "yes" ) );
        }
    }
    // Snapshot state right before autofire check.  If a grant set moves=92 in
    // an earlier iteration of this drain loop but a later message zeroed them,
    // we should see it here.  Deduped against the previous emission so an
    // idle/locked client (do_turn spinning at ~60Hz with no state change)
    // doesn't flood the log — fresh state changes still emit immediately.
    {
        const std::string msg = "[cdda-mp] CLI-DRAIN-END: moves=" +
                                std::to_string( get_avatar().get_moves() ) +
                                " ack=" + std::to_string( g_client_waiting_for_ack ) +
                                " last_seq=" + std::to_string( g_client_last_grant_seq ) +
                                " pending=" + ( g_pending_action.empty() ? "none" : "yes" );
        static std::string last;
        if( msg != last ) {
            mp_log( msg );
            last = msg;
        }
    }
    // Auto-fire any queued action now that the server has restored our moves.
    // Do NOT zero moves after firing — leave moves > 0 so the input loop runs
    // immediately after, giving the user the chance to queue the next action.
    // The ack guard (set below) prevents the input loop from double-sending.
    if( !g_pending_action.empty() && get_avatar().get_moves() <= 0 ) {
        // Diagnostic only: pending action exists but no moves to fire it.
        // Wait for the next grant — AUTOFIRE below will send on receipt.
        // No wedge-breaker: under pure lockstep, the host's grant cadence
        // is the rate limiter.  If grants aren't coming, the client must
        // not act — bypassing the grant lets the client desync from the
        // host's turn count.
        mp_log( "[cdda-mp] CLI-DEADZONE: pending=" + g_pending_action.substr( 0, 60 ) +
                " moves=" + std::to_string( get_avatar().get_moves() ) +
                " ack=" + std::to_string( g_client_waiting_for_ack ) +
                " last_seq=" + std::to_string( g_client_last_grant_seq ) );
    }
    if( !g_pending_action.empty() && get_avatar().get_moves() > 0 ) {
        mp_log( "[cdda-mp] CLI-AUTOFIRE: pending=" + g_pending_action.substr( 0, 60 ) +
                " moves=" + std::to_string( get_avatar().get_moves() ) +
                " ack=" + std::to_string( g_client_waiting_for_ack ) );
        // If we entered a wait activity AFTER queueing a move-style action,
        // the queued action is stale (user intent is now "wait").  Replace it
        // with a fresh wait so the host still receives our ack for this turn.
        // Under Option-A pure lockstep, every grant must be ack'd or the host
        // hits its 2s SAFETY-TIMEOUT and movement becomes sluggish.
        const player_activity &pact = get_avatar().activity;
        static const activity_id act_wait( "ACT_WAIT" );
        static const activity_id act_wait_stamina( "ACT_WAIT_STAMINA" );
        static const activity_id act_wait_weather( "ACT_WAIT_WEATHER" );
        static const activity_id act_wait_npc( "ACT_WAIT_NPC" );
        const bool in_wait_activity = pact && (
                pact.id() == act_wait || pact.id() == act_wait_stamina ||
                pact.id() == act_wait_weather || pact.id() == act_wait_npc );
        const bool pending_is_wait =
                g_pending_action.find( "\"action\":\"wait\"" ) != std::string::npos;
        if( in_wait_activity && !pending_is_wait ) {
            mp_log( "[cdda-mp] auto-fire: replacing stale action with wait (in " +
                    pact.id().str() + ")" );
            g_pending_action = client_enrich_action(
                    "{\"type\":\"action\",\"action\":\"wait\"}" );
        }
        mp_log( "[cdda-mp] CLI-AUTOFIRE-SEND: sending last_seq=" + std::to_string( g_client_last_grant_seq ) );
        client_send( g_pending_action );
        g_pending_action.clear();
        // Keep moves > 0: input loop will run so the user can queue the next action.
        // Ack guard prevents a second send before the server acknowledges this one.
        g_client_waiting_for_ack = true;
        g_ack_set_time = std::chrono::steady_clock::now();
    }
    // Warn if the client is drifting too far from the host's reality bubble center.
    // is_valid() rejects negative IDs but MP proxy NPCs have negative IDs — check
    // the raw value instead so the client-side warning actually fires.
    if( client_host_npc_id.get_value() != 0 ) {
        npc *hnpc = g->critter_by_id<npc>( client_host_npc_id );
        if( hnpc ) {
            check_separation_warning( get_avatar().pos_abs(), hnpc->pos_abs() );
        }
    }
    // Force a main-UI repaint whenever we processed any incoming messages.  The
    // client's main game loop stays in tight do_turn() iterations during long
    // activities — without an explicit redraw here, calendar/time/messages/tiles
    // updated by apply_one_state_message() never reach the screen until the user
    // happens to press a key.
    if( recv_count > 0 ) {
        g->invalidate_main_ui_adaptor();
        ui_manager::redraw();
        refresh_display();
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

            // Fields — baseline-gated.  Host owns field simulation (dust,
            // smoke, blood spread/decay); the only fields the host doesn't
            // already know about are ones the client *created* this turn via
            // an action.  Forwarding host-observed fields back to the host
            // was producing ~100 entries per turn while the partner sat in
            // ACT_WAIT and blocking the host's input pump.
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
            auto &field_baseline = g_client_field_baseline[abs];
            const bool fields_changed = ( field_baseline != fields_sig );
            if( fields_changed ) {
                field_baseline = fields_sig;
            }

            // Trap — baseline-gated. Skip terrain-builtin traps (e.g. downspout funnel
            // on t_gutter_downspout); the peer derives those from the terrain itself,
            // and re-applying via trap_set triggers a debugmsg.
            const trap &tr_here = m.tr_at( bub );
            const trap_id &builtin_here = m.ter( bub )->trap;
            const bool is_builtin_c = !tr_here.is_null() && tr_here.loadid == builtin_here;
            const std::string trap_sig_c = ( tr_here.is_null() || is_builtin_c ) ? "" : tr_here.id.str();
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

            if( !terfurn_changed && !items_changed && !fields_changed &&
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
            if( fields_changed ) {
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

// Scan vehicle cargo parts near the client avatar for item changes since the
// last action was sent.  Each entry identifies the cargo by its tile abs
// position; the host looks up the vehicle at that tile and updates its cargo.
// Mirrors build_client_tile_changes() for vehicle cargo storage so drops into
// trunks/freezers/lockers (and the SP fall-through to ground if the cargo is
// full) sync over the wire the same way ground drops do.
static std::string build_client_veh_cargo_changes( int radius = 12 )
{
    const avatar &av = get_avatar();
    const tripoint_abs_ms center = av.pos_abs();
    map &m = get_map();
    std::string out = "[";
    bool first = true;

    for( const wrapped_vehicle &wv : m.get_vehicles() ) {
        vehicle *v = wv.v;
        if( !v ) {
            continue;
        }
        // Cheap reject: skip vehicles whose root is far from the avatar.
        if( std::abs( v->pos_abs().x() - center.x() ) > radius + 20 ||
            std::abs( v->pos_abs().y() - center.y() ) > radius + 20 ||
            v->pos_abs().z() != center.z() ) {
            continue;
        }
        for( const vpart_reference &vp : v->get_any_parts( VPFLAG_CARGO ) ) {
            const tripoint_bub_ms vp_bub = vp.pos_bub( m );
            const tripoint_abs_ms vp_abs = m.get_abs( vp_bub );
            if( std::abs( vp_abs.x() - center.x() ) > radius ||
                std::abs( vp_abs.y() - center.y() ) > radius ) {
                continue;
            }
            std::string items_sig;
            std::string items_json = "[";
            bool ifirst = true;
            for( const item &it : v->get_items( vp.part() ) ) {
                const std::string item_json = serialize( it );
                items_sig += item_json + ',';
                if( !ifirst ) {
                    items_json += ',';
                }
                ifirst = false;
                items_json += item_json;
            }
            items_json += "]";
            auto &baseline = g_client_veh_cargo_baseline[vp_abs];
            if( baseline == items_sig ) {
                continue; // no change since last send
            }
            baseline = items_sig;
            mp_log( "[cdda-mp] client veh cargo @ " +
                    std::to_string( vp_abs.x() ) + "," +
                    std::to_string( vp_abs.y() ) + "," +
                    std::to_string( vp_abs.z() ) +
                    " items_sig_len=" + std::to_string( items_sig.size() ) );
            if( !first ) {
                out += ',';
            }
            first = false;
            out += "{\"x\":" + std::to_string( vp_abs.x() )
                   + ",\"y\":" + std::to_string( vp_abs.y() )
                   + ",\"z\":" + std::to_string( vp_abs.z() )
                   + ",\"items\":" + items_json + "}";
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

// Conjugate the first-word verb of `s` to third-person singular in place.
// "finish waiting" → "finishes waiting", "watch X" → "watches X".
// Inverse of fix_you_verb (which strips for host→client direction).
static void add_third_person_s( std::string &s )
{
    size_t end = s.find( ' ' );
    if( end == std::string::npos ) {
        end = s.size();
    }
    if( end == 0 || end > 40 ) {
        return;
    }
    const char last = s[end - 1];
    if( last == 's' ) {
        return;  // already conjugated
    }
    if( last == 'x' || last == 'z' ) {
        s.insert( end, "es" );
        return;
    }
    if( end >= 2 ) {
        const std::string two = s.substr( end - 2, 2 );
        if( two == "ch" || two == "sh" ) {
            s.insert( end, "es" );
            return;
        }
    }
    s.insert( end, "s" );
}

// Snapshot any new "You ..." messages the client's avatar produced since the
// last send.  Substitute "You" with the client's character name so the host
// reads them in third person ("Roy finishes waiting", "Roy is now reading X").
// Drains into the enriched action payload below.
static void client_capture_avatar_msgs()
{
    const size_t cur = Messages::size();
    if( cur <= g_client_msg_watermark ) {
        g_client_msg_watermark = cur;
        return;
    }
    const auto new_msgs = Messages::recent_messages( cur - g_client_msg_watermark );
    g_client_msg_watermark = cur;
    const std::string client_name = get_avatar().name;
    for( const auto &[time_str, text] : new_msgs ) {
        ( void )time_str;
        if( text.rfind( "You ", 0 ) != 0 && text.rfind( "Now ", 0 ) != 0 ) {
            continue;  // skip ambient/UI/inventory chatter
        }
        std::string out = text;
        if( out.rfind( "You ", 0 ) == 0 ) {
            // "You finish waiting" → "finish waiting" → "finishes waiting" → "Roy finishes waiting"
            std::string rest = out.substr( 4 );
            add_third_person_s( rest );
            out = client_name + " " + rest;
        } else {
            // "Now reading X" → "Roy is now reading X"
            out = client_name + " is " + out;
        }
        g_client_msgs_pending.push_back( out );
    }
}

std::string client_enrich_action( const std::string &json )
{
    const avatar &av = get_avatar();
    client_capture_avatar_msgs();

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
    const std::string veh_cargo_changes = build_client_veh_cargo_changes();

    // Worn-list baseline check: if the avatar's worn list (or wielded item)
    // changed since the last send, fire a worn_sync packet so the host's NPC
    // proxy reflects the new worn state.  Captures drops that peel garments
    // off, take_off, wear, and any other worn-mutating activity.  Baseline
    // includes type + variant + wielded id so a swap (drop X, wear Y) flips
    // the signature.
    {
        std::vector<const item *> worn_items;
        av.worn.inv_dump( worn_items );
        std::string worn_sig;
        for( const item *it : worn_items ) {
            worn_sig += it->typeId().str();
            if( it->has_itype_variant() ) {
                worn_sig += '|';
                worn_sig += it->itype_variant().id;
            }
            worn_sig += ',';
        }
        item_location wielded = av.get_wielded_item();
        worn_sig += ';';
        if( wielded ) {
            worn_sig += wielded->typeId().str();
        }
        if( worn_sig != g_client_worn_baseline ) {
            g_client_worn_baseline = worn_sig;
            client_resync_worn();
        }
    }

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
        if( veh_cargo_changes != "[]" ) {
            enriched += ",\"client_veh_cargo_changes\":" + veh_cargo_changes;
        }
        enriched += ",\"client_stamina\":" + std::to_string( av.get_stamina() );
        const std::string monster_hits = build_client_monster_hits();
        if( !monster_hits.empty() ) {
            enriched += ",\"client_monster_hits\":" + monster_hits;
        }
        enriched += ",\"char_stats\":" + char_stats;
        enriched += ",\"client_facing\":" + std::to_string(
                        av.facing == FacingDirection::LEFT ? 0 : 1 );
        // Sync current activity id for HUD/messaging and as a heartbeat for
        // the host's lockstep bypass.  Primary signals are the explicit
        // activity_start / activity_end actions emitted in assign_activity /
        // do_turn — this field is belt-and-suspenders so a missed lifecycle
        // packet still gets reconciled on the next normal action.  Refresh
        // from live av.activity when present so an activity assigned mid-turn
        // is reflected here too.
        if( av.activity ) {
            g_client_turn_activity = av.activity.id().str();
        }
        const std::string client_act_id = g_client_turn_activity;
        enriched += ",\"client_activity\":\"" + client_act_id + "\"";
        // Progress percentage of the live activity, for the host's Co-op panel.
        // mp_compute_activity_pct handles crafting (item_counter-based) as
        // well as standard moves_total-based activities.
        enriched += ",\"client_activity_pct\":" + std::to_string(
                        mp_compute_activity_pct( av.activity ) );
        // Total moves of the live activity, so the host's bump menu can gate
        // the "Help with task" option on "long enough to warrant it".
        enriched += ",\"client_activity_moves_total\":" + std::to_string(
                        av.activity ? av.activity.moves_total : 0 );
        // Local calendar turn so the host can show a sync-drift indicator.
        enriched += ",\"client_calendar_turn\":" + std::to_string(
                        to_turn<int>( calendar::turn ) );
        if( !g_client_msgs_pending.empty() ) {
            std::string msgs = "[";
            bool first_m = true;
            for( const std::string &m : g_client_msgs_pending ) {
                if( !first_m ) {
                    msgs += ',';
                }
                first_m = false;
                msgs += "\"" + json_escape_str( m ) + "\"";
            }
            msgs += "]";
            enriched += ",\"client_msgs\":" + msgs;
            g_client_msgs_pending.clear();
        }
        enriched += '}';
    }
    return enriched;
}

void client_queue_action( const std::string &json )
{
    g_pending_action = client_enrich_action( json );
    mp_log( "[cdda-mp] client_queue_action: " + json.substr( 0, 80 ) );
}

void set_client_turn_activity( const std::string &activity_id_str )
{
    g_client_turn_activity = activity_id_str;
}

const std::string &get_client_turn_activity()
{
    return g_client_turn_activity;
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
    const std::string ma_style_str = av.martial_arts_data->selected_style().str();
    worn_json += "],\"male\":" + male_str
                 + ",\"appearance\":" + appearance_json
                 + ",\"wielded\":\"" + wielded_type + "\""
                 + ",\"ma_style\":\"" + ma_style_str + "\"}";
    client_send( worn_json );
}

void client_mark_action_sent()
{
    mp_log( "[cdda-mp] CLI-ACK-SET: ack guard SET, was=" + std::to_string( g_client_waiting_for_ack ) +
            " last_seq=" + std::to_string( g_client_last_grant_seq ) );
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
    // Fast-forward mode: when both sides are in passive activities, host has
    // dropped its strict-lockstep wait (see wait_for_client_action).  We
    // skip the wait dispatch too — sending one would just queue against the
    // host's already-bypassed wait and waste a TCP round-trip.  The host's
    // next grant arrives at near-SP-tick speed and ticks our activity locally.
    if( should_fast_forward() ) {
        mp_log( "[cdda-mp] dispatch_wait: FAST-FORWARD, skip wait for act=" +
                ( id ? id.str() : "idle" ) );
        return;
    }
    mp_log( "[cdda-mp] dispatch_wait: SEND wait for act=" + ( id ? id.str() : "idle" ) );
    client_send( client_enrich_action( "{\"type\":\"action\",\"action\":\"wait\"}" ) );
    client_mark_action_sent();
}

// Signal-only lifecycle markers.  Bypass enrich (no stat blob needed) and do
// NOT set the ack guard — these are out-of-band notifications that don't
// participate in the grant/wait/ack cycle.  The host treats them as pure
// state-machine inputs that flip g_partner_activity.
void client_send_activity_start( const std::string &activity_id_str )
{
    if( !is_client_mode() || activity_id_str.empty() ) {
        return;
    }
    const std::string json = "{\"type\":\"action\",\"action\":\"activity_start\","
                             "\"activity_id\":\"" + activity_id_str + "\"}";
    const player_activity &cur = get_avatar().activity;
    mp_log( "[cdda-mp] ACT-START SEND: id=" + activity_id_str
            + " g_client_turn_activity=" + g_client_turn_activity
            + " av.activity=" + ( cur ? cur.id().str() : "none" )
            + " moves=" + std::to_string( get_avatar().get_moves() ) );
    client_send( json );
}

void client_send_activity_end( const std::string &activity_id_str )
{
    if( !is_client_mode() ) {
        return;
    }
    const std::string json = "{\"type\":\"action\",\"action\":\"activity_end\","
                             "\"activity_id\":\"" + activity_id_str + "\"}";
    const player_activity &cur = get_avatar().activity;
    mp_log( "[cdda-mp] ACT-END SEND: id=" + activity_id_str
            + " g_client_turn_activity=" + g_client_turn_activity
            + " av.activity=" + ( cur ? cur.id().str() : "none" )
            + " ack=" + std::to_string( g_client_waiting_for_ack )
            + " moves=" + std::to_string( get_avatar().get_moves() ) );
    client_send( json );
}

// Compute the tile baseline signature from current authoritative map state.
// Used by build_tile_changes to detect change vs last broadcast, and by the
// client_tile_changes apply handler to refresh the baseline after applying
// client-supplied state — without that refresh, the next build_tile_changes
// would re-broadcast the same state back, causing an item/field round-trip
// loop with the client (uids drift through deserialize → broadcast → apply,
// fueling a per-turn 80 KB ping-pong while the partner sits in ACT_WAIT).
static mp_tile_state compute_tile_state( const tripoint_abs_ms &abs )
{
    mp_tile_state st;
    map &m = get_map();
    if( !m.inbounds( abs ) ) {
        return st;
    }
    const tripoint_bub_ms bub = m.get_bub( abs );

    st.ter  = m.ter( bub ).id().str();
    st.furn = m.furn( bub ).id().str();

    for( const item &it : m.i_at( bub ) ) {
        st.items_sig += serialize( it ) + ',';
    }

    const field &fld = m.field_at( bub );
    if( fld.field_count() > 0 ) {
        for( const auto &[ftype, fentry] : fld ) {
            if( !fentry.is_field_alive() ) {
                continue;
            }
            st.fields_sig += ftype.id().str() + ':' +
                             std::to_string( fentry.get_field_intensity() ) + ',';
        }
    }

    const trap &tr = m.tr_at( bub );
    const trap_id &builtin = m.ter( bub )->trap;
    const bool is_builtin = !tr.is_null() && tr.loadid == builtin;
    st.trap_sig = ( tr.is_null() || is_builtin ) ? "" : tr.id.str();

    st.graffiti_sig = m.has_graffiti_at( bub ) ? m.graffiti_at( bub ) : "";

    return st;
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

            // Trap — id string, empty when no placed trap (tr_null) or when the
            // trap is just the terrain's built-in (peer derives those from the
            // terrain itself; re-applying via trap_set triggers a debugmsg).
            const trap &tr      = m.tr_at( bub );
            const trap_id &builtin = m.ter( bub )->trap;
            const bool is_builtin = !tr.is_null() && tr.loadid == builtin;
            const std::string trap_sig = ( tr.is_null() || is_builtin ) ? "" : tr.id.str();

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
            // Skip if the terrain has a built-in trap (e.g. downspout funnel on
            // t_gutter_downspout); trap_set refuses any trap on such tiles and
            // debugmsgs.  Peer derives the trap from the terrain.
            const trap_id &builtin = m.ter( bub )->trap;
            if( builtin != tr_null ) {
                // no-op
            } else if( trap_str.empty() || trap_str == "tr_null" ) {
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

        // Refresh client→server baselines to match the state we just installed
        // from the host.  Without this, the next build_client_tile_changes
        // re-serializes the items/ter/etc., compares to a stale baseline,
        // marks "changed", and ships them back to the host — fueling an 80 KB
        // per-turn round-trip while the avatar sits in ACT_WAIT.  Computed
        // from current map state (not the JSON we received) so the sig
        // matches what build_client_tile_changes will produce.
        {
            const ter_id &cur_ter = m.ter( bub );
            const furn_id &cur_furn = m.furn( bub );
            g_client_terfurn_baseline[abs] = cur_ter.id().str() + '|' + cur_furn.id().str();

            std::string cur_items_sig;
            for( const item &it : m.i_at( bub ) ) {
                cur_items_sig += serialize( it ) + ',';
            }
            g_client_item_baseline[abs] = cur_items_sig;

            std::string cur_fields_sig;
            const field &cur_fld = m.field_at( bub );
            if( cur_fld.field_count() > 0 ) {
                for( const auto &[ftype, fentry] : cur_fld ) {
                    if( !fentry.is_field_alive() ) {
                        continue;
                    }
                    cur_fields_sig += ftype.id().str() + ':' +
                                      std::to_string( fentry.get_field_intensity() ) + ',';
                }
            }
            g_client_field_baseline[abs] = cur_fields_sig;

            const trap &cur_tr = m.tr_at( bub );
            const trap_id &cur_builtin = m.ter( bub )->trap;
            const bool cur_is_builtin = !cur_tr.is_null() && cur_tr.loadid == cur_builtin;
            g_client_trap_baseline[abs] =
                ( cur_tr.is_null() || cur_is_builtin ) ? "" : cur_tr.id.str();

            g_client_graffiti_baseline[abs] =
                m.has_graffiti_at( bub ) ? m.graffiti_at( bub ) : "";
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

    // Process host-reported removals first: any nid the host no longer tracks
    // (folded, fully destroyed, driven out of bubble) should be torn down on
    // the client to mirror SP's map::destroy_vehicle.  Done before the apply
    // loop so a vehicle removed-and-replaced in the same broadcast doesn't
    // collide with its successor at the same tile.
    if( jo.has_array( "removed_vehicles" ) ) {
        for( const JsonValue &rv : jo.get_array( "removed_vehicles" ) ) {
            const auto rnid = static_cast<uint32_t>( rv.get_int() );
            auto pos_it = g_client_veh_pos.find( rnid );
            if( pos_it == g_client_veh_pos.end() ) {
                continue;
            }
            const tripoint_abs_ms rabs = pos_it->second;
            g_client_veh_pos.erase( pos_it );
            if( !m.inbounds( rabs ) ) {
                continue;
            }
            const optional_vpart_position vp = m.veh_at( m.get_bub( rabs ) );
            if( !vp ) {
                continue;
            }
            vehicle &dveh = vp->vehicle();
            mp_log( "[cdda-mp] CLI-VEH-REMOVE: nid=" + std::to_string( rnid )
                    + " abs=" + std::to_string( rabs.x() )
                    + "," + std::to_string( rabs.y() )
                    + "," + std::to_string( rabs.z() )
                    + " name=\"" + dveh.name + "\"" );
            m.destroy_vehicle( &dveh );
        }
    }

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

        vehicle *found = nullptr;

        // Snapshot path: the host emits a full save-format snapshot for this
        // nid on first encounter AND any time the parts vector mutates
        // (install, remove, fold, damage-purge).  Treat it as an authoritative
        // replacement: destroy any prior local instance for this nid, then
        // deserialize-and-place.  The snapshot is complete state, so we skip
        // the slim per-part / cargo apply that follows and move on.
        if( vo.has_object( "snapshot" ) && m.inbounds( new_abs ) ) {
            // Tear down the previous local instance (if any) so a structural
            // change doesn't end up with two overlapping vehicles at the same
            // tile.  Look up by tracked position; fall back to scanning by name.
            auto prev_it = g_client_veh_pos.find( nid );
            tripoint_abs_ms prev_abs = ( prev_it != g_client_veh_pos.end() )
                                       ? prev_it->second
                                       : new_abs;
            vehicle *prev = nullptr;
            if( m.inbounds( prev_abs ) ) {
                if( const optional_vpart_position vp = m.veh_at( m.get_bub( prev_abs ) ) ) {
                    prev = &vp->vehicle();
                }
            }
            if( !prev && !vname.empty() ) {
                for( const wrapped_vehicle &wv : vehs ) {
                    if( wv.v && wv.v->name == vname ) {
                        prev = wv.v;
                        break;
                    }
                }
            }
            if( prev ) {
                mp_log( "[cdda-mp] CLI-VEH-REPLACE: nid=" + std::to_string( nid )
                        + " name=\"" + prev->name + "\"" );
                m.destroy_vehicle( prev );
            }
            if( prev_it != g_client_veh_pos.end() ) {
                g_client_veh_pos.erase( prev_it );
            }

            JsonObject snap = vo.get_object( "snapshot" );
            snap.allow_omitted_members();
            static const vproto_id vehicle_prototype_none( "none" );
            auto veh_up = std::make_unique<vehicle>( vehicle_prototype_none );
            veh_up->deserialize( snap );
            // Re-anchor sm_pos+pos to the broadcast abs position — the
            // serialized "posx"/"posy" are submap-relative and need to match
            // the client's bubble origin.  Mirrors map::add_vehicle().
            const tripoint_bub_ms new_bub = m.get_bub( new_abs );
            tripoint_bub_sm quotient;
            point_sm_ms remainder;
            std::tie( quotient, remainder ) =
                coords::project_remain<coords::sm>( new_bub );
            veh_up->sm_pos = m.get_abs_sub().xy() + rebase_rel( quotient );
            veh_up->pos = remainder;
            // Preserve the deserialized pivot_anchor / pivot_rotation — those
            // were set by vehicle::deserialize from the snapshot's "pivot" and
            // "faceDir" fields, and reflect the host's authoritative state.
            // Passing point_rel_ms::zero here (as map::add_vehicle does for a
            // freshly-spawned vehicle from a prototype) would clobber any
            // non-zero pivot the host has accumulated through rotations or
            // part changes, shifting every part by a fixed offset relative
            // to pos_abs.  pos_abs matches the host's, but the rendered
            // tiles end up several squares off — visible as the bus
            // appearing at the wrong location on the client after load.
            veh_up->precalc_mounts( 0, veh_up->pivot_rotation[0],
                                    veh_up->pivot_anchor[0] );
            vehicle *placed = m.add_vehicle_from_snapshot( std::move( veh_up ) );
            if( !placed ) {
                mp_log( "[cdda-mp] CLI-VEH-CREATE-FAIL: nid=" + std::to_string( nid )
                        + " abs=" + std::to_string( new_abs.x() )
                        + "," + std::to_string( new_abs.y() )
                        + "," + std::to_string( new_abs.z() ) );
                continue;
            }
            g_client_veh_pos[nid] = placed->pos_abs();
            mp_log( "[cdda-mp] CLI-VEH-CREATE: nid=" + std::to_string( nid )
                    + " abs=" + std::to_string( new_abs.x() )
                    + "," + std::to_string( new_abs.y() )
                    + "," + std::to_string( new_abs.z() )
                    + " name=\"" + placed->name + "\"" );
            // Snapshot is fully authoritative — no need to re-apply slim deltas.
            continue;
        }

        // Slim path: no snapshot.  Find the existing local vehicle by tracked
        // position, server's new_abs, or name.  Same fallback chain as before.
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
        if( !found && m.inbounds( new_abs ) ) {
            for( const wrapped_vehicle &wv : vehs ) {
                if( wv.v && wv.v->pos_abs() == new_abs ) {
                    found = wv.v;
                    break;
                }
            }
        }
        if( !found && !vname.empty() ) {
            for( const wrapped_vehicle &wv : vehs ) {
                if( wv.v && wv.v->name == vname ) {
                    found = wv.v;
                    break;
                }
            }
        }

        if( !found ) {
            // No local vehicle and no snapshot in this packet (slim
            // vehicle_step before first state, or out-of-bounds).  Skip and
            // wait for the next full state broadcast which will carry one.
            mp_log( "[cdda-mp] CLI-VEH-SKIP-UNKNOWN: nid=" + std::to_string( nid )
                    + " new_abs=" + std::to_string( new_abs.x() )
                    + "," + std::to_string( new_abs.y() )
                    + "," + std::to_string( new_abs.z() )
                    + " name=\"" + vname + "\""
                    + " first_encounter=" + std::to_string( first_encounter ) );
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

        // Apply per-cargo-part item lists the host sent.  Position is already
        // synced above so m.veh_at(vp_bub).cargo() resolves to the correct part.
        // Mirrors the server-side client_veh_cargo_changes applier so client and
        // host converge on the same items_sig the next time either side scans.
        if( vo.has_array( "cargo" ) ) {
            for( const JsonValue &cv : vo.get_array( "cargo" ) ) {
                JsonObject co = const_cast<JsonValue &>( cv ).get_object();
                co.allow_omitted_members();
                const tripoint_abs_ms vp_abs{
                    co.get_int( "x" ), co.get_int( "y" ), co.get_int( "z" )
                };
                if( !m.inbounds( vp_abs ) ) {
                    continue;
                }
                const tripoint_bub_ms vp_bub = m.get_bub( vp_abs );
                const std::optional<vpart_reference> cargo_vp = m.veh_at( vp_bub ).cargo();
                if( !cargo_vp ) {
                    mp_log( "[cdda-mp] client veh cargo miss: no cargo part at " +
                            std::to_string( vp_abs.x() ) + "," +
                            std::to_string( vp_abs.y() ) + "," +
                            std::to_string( vp_abs.z() ) );
                    continue;
                }
                vehicle &veh = cargo_vp->vehicle();
                vehicle_part &part = cargo_vp->part();
                mp_log( "[cdda-mp] client apply veh cargo @ " +
                        std::to_string( vp_abs.x() ) + "," +
                        std::to_string( vp_abs.y() ) + "," +
                        std::to_string( vp_abs.z() ) );
                {
                    vehicle_stack stack = veh.get_items( part );
                    while( !stack.empty() ) {
                        stack.erase( stack.begin() );
                    }
                }
                std::string items_sig;
                if( co.has_array( "items" ) ) {
                    for( const JsonValue &iv : co.get_array( "items" ) ) {
                        try {
                            item new_item;
                            JsonObject io = const_cast<JsonValue &>( iv ).get_object();
                            io.allow_omitted_members();
                            new_item.deserialize( io );
                            if( !new_item.typeId().is_empty() &&
                                new_item.typeId().is_valid() ) {
                                veh.add_item( m, part, new_item );
                                items_sig += serialize( new_item ) + ',';
                            }
                        } catch( const JsonError & ) {}
                    }
                }
                // Resync the client→host baseline to the post-apply state so the
                // next build_client_veh_cargo_changes() doesn't immediately
                // re-send the host's authoritative contents back as a "client
                // delta" (which would be a no-op but pollutes the wire).
                g_client_veh_cargo_baseline[vp_abs] = items_sig;
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
    mp_log( "[cdda-mp] SRP: npc_abs=" + std::to_string( pos.x() ) + "," + std::to_string( pos.y() ) +
            " bub=" + std::to_string( pos_bub.x() ) + "," + std::to_string( pos_bub.y() ) );
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
    std::unordered_set<uint32_t> alive_now;
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
            alive_now.insert( nid );

            // Detect structural change: if the parts vector grew or shrank since
            // the last broadcast (install, remove, damage-purge, fold-merge), the
            // sequential part indices the slim sync relies on no longer line up
            // with the client's local vector.  Drop the nid from the snapshot
            // tracking so the very next broadcast re-emits the full snapshot
            // and the client mirrors the new structural state via the existing
            // CLI-VEH-CREATE path.  Use part_count_real() — part_count()
            // includes fake parts that are added/removed transiently by
            // refresh()/remove_fake_parts() between broadcasts (e.g. for
            // turn-rendering), which would oscillate the baseline and trigger
            // a snapshot resend on every turn, looping client+host indefinitely.
            const size_t parts_now = v->part_count_real();
            auto pc_it = g_server_veh_parts_count.find( nid );
            if( pc_it != g_server_veh_parts_count.end() && pc_it->second != parts_now ) {
                mp_log( "[cdda-mp] HOST-VEH-PARTS-CHANGED: nid=" + std::to_string( nid )
                        + " was=" + std::to_string( pc_it->second )
                        + " now=" + std::to_string( parts_now )
                        + " name=\"" + v->name + "\"" );
                g_client_known_veh_nids.erase( nid );
            }
            g_server_veh_parts_count[nid] = parts_now;
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

            // Per-cargo-part contents.  Baseline-diff gated so a 100-item trunk
            // with stable contents costs nothing on the wire — we only emit
            // parts whose items actually changed since the last broadcast.
            // Mirrors build_client_veh_cargo_changes() in the opposite direction.
            std::string cargo_json = "[";
            bool cfirst = true;
            for( const vpart_reference &vpr : v->get_any_parts( VPFLAG_CARGO ) ) {
                const tripoint_abs_ms vp_abs = vmap.get_abs( vpr.pos_bub( vmap ) );
                std::string items_sig;
                std::string items_json = "[";
                bool ifirst = true;
                for( const item &it : v->get_items( vpr.part() ) ) {
                    const std::string item_json = serialize( it );
                    items_sig += item_json + ',';
                    if( !ifirst ) {
                        items_json += ',';
                    }
                    ifirst = false;
                    items_json += item_json;
                }
                items_json += "]";
                auto &baseline = g_host_veh_cargo_baseline[vp_abs];
                if( baseline == items_sig ) {
                    continue; // no change since last broadcast
                }
                baseline = items_sig;
                mp_log( "[cdda-mp] host veh cargo @ " +
                        std::to_string( vp_abs.x() ) + "," +
                        std::to_string( vp_abs.y() ) + "," +
                        std::to_string( vp_abs.z() ) +
                        " items_sig_len=" + std::to_string( items_sig.size() ) );
                if( !cfirst ) {
                    cargo_json += ',';
                }
                cfirst = false;
                cargo_json += "{\"x\":" + std::to_string( vp_abs.x() )
                              + ",\"y\":" + std::to_string( vp_abs.y() )
                              + ",\"z\":" + std::to_string( vp_abs.z() )
                              + ",\"items\":" + items_json + "}";
            }
            cargo_json += ']';

            if( !vfirst ) {
                vehicles_json += ',';
            }
            vfirst = false;
            // First-encounter snapshot: include the full save-format vehicle
            // JSON so the client can mirror SP's add_vehicle / save-load path
            // for vehicles it has no local instance of (debug spawn, deployed
            // foldable, mid-game-constructed frame, mapgen-into-bubble).  The
            // snapshot is only emitted once per nid per client connection;
            // subsequent broadcasts stay slim.  Field name "snapshot" is a
            // nested JSON object so no escaping is needed.
            std::string snapshot_field;
            if( !g_client_known_veh_nids.count( nid ) ) {
                snapshot_field = ",\"snapshot\":" + ::serialize( *v );
                g_client_known_veh_nids.insert( nid );
                mp_log( "[cdda-mp] HOST-VEH-SNAPSHOT: nid=" + std::to_string( nid )
                        + " name=\"" + v->name + "\" bytes="
                        + std::to_string( snapshot_field.size() ) );
            }
            vehicles_json += "{\"nid\":" + std::to_string( nid )
                             + ",\"x\":" + std::to_string( vabs.x() )
                             + ",\"y\":" + std::to_string( vabs.y() )
                             + ",\"z\":" + std::to_string( vabs.z() )
                             + ",\"face\":" + std::to_string( face_deg )
                             + ",\"turn_dir\":" + std::to_string( turn_dir_deg )
                             + ",\"vel\":" + std::to_string( v->velocity )
                             + ",\"cruise\":" + std::to_string( v->cruise_velocity )
                             + ",\"name\":\"" + vname_escaped + "\""
                             + ",\"parts\":" + parts_json
                             + ",\"cargo\":" + cargo_json
                             + snapshot_field + "}";
        }
    }
    vehicles_json += ']';

    // Diff against the previous broadcast: any nid that was live last time but
    // isn't in alive_now has disappeared (folded, fully destroyed, driven out
    // of bubble, etc.).  Emit a removed_vehicles array so the client can
    // mirror SP's map::destroy_vehicle path.  Also drop those nids from the
    // snapshot/parts-count tracking so a future re-encounter triggers a fresh
    // snapshot.
    std::string removed_vehicles_json = "[";
    {
        bool rfirst = true;
        for( const uint32_t old_nid : g_server_veh_live_nids ) {
            if( alive_now.count( old_nid ) ) {
                continue;
            }
            if( !rfirst ) {
                removed_vehicles_json += ',';
            }
            rfirst = false;
            removed_vehicles_json += std::to_string( old_nid );
            g_client_known_veh_nids.erase( old_nid );
            g_server_veh_parts_count.erase( old_nid );
            mp_log( "[cdda-mp] HOST-VEH-REMOVED: nid=" + std::to_string( old_nid ) );
        }
    }
    removed_vehicles_json += ']';
    g_server_veh_live_nids = std::move( alive_now );

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
           "\"host_in_vehicle\":" + std::string( host.in_vehicle ? "true" : "false" ) + ","
           "\"host_ctrl_veh\":" + std::string( host.controlling_vehicle ? "true" : "false" ) + ","
           "\"host_activity\":\"" + ( host.activity ? host.activity.id().str() : "" ) + "\","
           "\"host_activity_pct\":" + std::to_string(
               mp_compute_activity_pct( host.activity ) ) + ","
           "\"host_activity_moves_total\":" + std::to_string(
               host.activity ? host.activity.moves_total : 0 ) + ","
           + ( []() -> std::string {
               // One-shot wake_client signal — emit on this broadcast then clear.
               if( g_pending_wake_client ) {
                   g_pending_wake_client = false;
                   return "\"wake_client\":true,";
               }
               return "";
           }() ) +
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
           ",\"removed_vehicles\":" + removed_vehicles_json +
           ",\"msgs\":" + msgs_json +
           ",\"grant_seq\":" + std::to_string( g_grant_seq ) +
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
