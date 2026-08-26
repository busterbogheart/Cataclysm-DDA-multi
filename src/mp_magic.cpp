#include "mp_magic.h"

#include <map>
#include <set>
#include <string>
#include <utility>

#include "bodypart.h"
#include "line.h"

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "effect.h"
#include "coordinates.h"
#include "event.h"
#include "event_bus.h"
#include "event_subscriber.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "json_loader.h"
#include "magic.h"
#include "map.h"
#include "monster.h"
#include "mp_client_conn.h"
#include "mp_gamestate.h"
#include "mp_server.h"
#include "mtype.h"
#include "npc.h"
#include "translations.h"
#include "type_id.h"

namespace cata_mp
{

namespace
{

// Turn on which this instance last resolved a spell, and its id.  Deliberately
// a plain global rather than saved state — it exists only to give the HP
// applier a "was this near a cast?" discriminator within a session.
int g_last_cast_turn = -1;
std::string g_last_cast_spell;

std::string role_tag()
{
    if( is_hosting() ) {
        return "HOST";
    }
    if( is_client_mode() ) {
        return "CLIENT";
    }
    return "SOLO";
}

// Compact per-bodypart HP line.  Logged at cast resolution so the NEXT
// host->client bodyparts sync can be diffed against what the caster actually
// had immediately after their own spell resolved.  Without this snapshot the
// applier's "host says X, we have Y" line has nothing to attribute Y to.
std::string hp_snapshot( const Character &who )
{
    std::string out;
    for( const bodypart_id &bp : who.get_all_body_parts() ) {
        if( !out.empty() ) {
            out += ',';
        }
        out += bp.id().str() + ':' + std::to_string( who.get_part_hp_cur( bp ) );
    }
    return out;
}

// Effect ids currently on the caster.  Client self-buffs are the single largest
// spell category in Magiclysm (283 of 693 spells target self only) and none of
// them reach the host's proxy today, so this is the before-picture for the
// effect-sync work (ROADMAP B3).
std::string effect_ids( const Character &who )
{
    std::string out;
    for( const effect &eff : who.get_effects() ) {
        if( !out.empty() ) {
            out += ',';
        }
        out += eff.get_id().str();
    }
    return out.empty() ? "none" : out;
}

class mp_magic_events : public event_subscriber
{
    public:
        void notify( const cata::event &e ) override {
            // Only instrument when we're actually in a co-op session; solo play
            // shouldn't pay for this or pollute the log.
            if( !is_hosting() && !is_client_mode() ) {
                return;
            }
            switch( e.type() ) {
                case event_type::spellcasting_finish:
                    on_cast_finish( e );
                    break;
                case event_type::player_levels_spell:
                    on_level_up( e );
                    break;
                default:
                    break;
            }
        }

    private:
        static void on_cast_finish( const cata::event &e ) {
            const character_id who_id = e.get<character_id>( "character" );
            Character *who = g->critter_by_id<Character>( who_id );
            const bool success = e.get<cata_variant_type::bool_>( "success" );
            const spell_id sp = e.get<spell_id>( "spell" );
            const trait_id school = e.get<trait_id>( "school" );

            // Note this even on failure: a failed cast still burns 20% of the
            // mana (magic_type::failure_cost_percent) and still runs the
            // failure EOCs, both of which are client-local today.
            std::string line = "[cdda-mp] MAGIC-CAST " + role_tag() +
                               ": spell=" + sp.str() +
                               " school=" + school.str() +
                               " success=" + std::to_string( success ) +
                               " diff=" + std::to_string( e.get<cata_variant_type::int_>( "difficulty" ) ) +
                               " cost=" + std::to_string( e.get<cata_variant_type::int_>( "cost" ) ) +
                               " cast_time=" + std::to_string( e.get<cata_variant_type::int_>( "cast_time" ) ) +
                               " dmg=" + std::to_string( e.get<cata_variant_type::int_>( "damage" ) ) +
                               " turn=" + std::to_string( to_turn<int>( calendar::turn ) );

            if( who != nullptr ) {
                line += " caster=" + who->name +
                        ( who->is_avatar() ? "(self)" : "(other)" ) +
                        " mana=" + std::to_string( who->magic->available_mana() ) +
                        "/" + std::to_string( who->magic->max_mana( *who ) ) +
                        " hp=[" + hp_snapshot( *who ) + "]" +
                        " effects=[" + effect_ids( *who ) + "]";
                // Only our own avatar's casts can desync — an NPC casting on
                // the host is already authoritative.
                if( who->is_avatar() ) {
                    g_last_cast_turn = to_turn<int>( calendar::turn );
                    g_last_cast_spell = sp.str();
                }
            } else {
                line += " caster=<not found>";
            }
            mp_log( line );
        }

        static void on_level_up( const cata::event &e ) {
            mp_log( "[cdda-mp] MAGIC-LEVEL " + role_tag() +
                    ": spell=" + e.get<spell_id>( "spell" ).str() +
                    " new_level=" + std::to_string( e.get<cata_variant_type::int_>( "new_level" ) ) );
        }
};

mp_magic_events &magic_events()
{
    static mp_magic_events subscriber;
    return subscriber;
}

} // namespace

void mp_subscribe_magic_events( event_bus &bus )
{
    bus.subscribe( &magic_events() );
}

bool mp_is_summoned( const Creature &c )
{
    // get_summoner() is non-const in Creature; the cast is safe because we only
    // read the pointer.  A summon carries a summoner even after its timer is
    // cleared by the PERMANENT flag, so this is the reliable discriminator.
    return const_cast<Creature &>( c ).get_summoner() != nullptr;
}

int mp_turns_since_last_cast()
{
    if( g_last_cast_turn < 0 ) {
        return -1;
    }
    return to_turn<int>( calendar::turn ) - g_last_cast_turn;
}

std::string mp_last_cast_spell()
{
    return g_last_cast_spell.empty() ? std::string( "none" ) : g_last_cast_spell;
}

void mp_on_summon_placed( monster &mon, int summon_turns, bool permanent )
{
    if( !is_client_mode() ) {
        return;   // host is already authoritative; nothing to do
    }
    const tripoint_abs_ms abs = mon.pos_abs();
    const std::string payload =
        std::string( "{\"type\":\"client_summon\",\"mtype\":\"" ) + mon.type->id.str() +
        "\",\"x\":" + std::to_string( abs.x() ) +
        ",\"y\":" + std::to_string( abs.y() ) +
        ",\"z\":" + std::to_string( abs.z() ) +
        ",\"friendly\":" + std::to_string( mon.friendly ) +
        ",\"turns\":" + std::to_string( permanent ? 0 : summon_turns ) +
        ",\"no_drops\":" + ( mon.no_extra_death_drops ? "true" : "false" ) +
        ",\"quiet\":" + ( mon.no_corpse_quiet ? "true" : "false" ) + "}";
    mp_log( "[cdda-mp] CLI-SUMMON-SEND: " + payload );
    client_send( payload );
    // Drop our copy immediately.  Keeping it would either double-spawn when the
    // host's real one arrives, or be culled anyway before the round-trip
    // completes -- the exact failure this is fixing.  remove_zombie despawns
    // cleanly with no corpse, which is what the cull path itself uses.
    g->remove_zombie( mon );
}

void mp_handle_client_summon( const std::string &msg )
{
    if( !is_hosting() ) {
        return;
    }
    std::string mtype_str;
    tripoint_abs_ms abs;
    int friendly = 0;
    int turns = 0;
    bool no_drops = true;
    bool quiet = false;
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        mtype_str = jo.get_string( "mtype", "" );
        abs = tripoint_abs_ms{ jo.get_int( "x", 0 ), jo.get_int( "y", 0 ), jo.get_int( "z", 0 ) };
        friendly = jo.get_int( "friendly", 0 );
        turns = jo.get_int( "turns", 0 );
        no_drops = jo.get_bool( "no_drops", true );
        quiet = jo.get_bool( "quiet", false );
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] HOST-SUMMON parse error: " ) + e.what() );
        return;
    }
    const mtype_id mid( mtype_str );
    if( mtype_str.empty() || !mid.is_valid() ) {
        mp_log( "[cdda-mp] HOST-SUMMON: REJECTED, invalid mtype '" + mtype_str + "'" );
        return;
    }
    map &here = get_map();
    if( !here.inbounds( abs ) ) {
        // Out of the host's bubble.  The client already paid the mana and the
        // components, so this is a real loss for them -- log it loudly rather
        // than dropping silently, because it is the signature of the pair being
        // too far apart rather than of a bug in this path.
        mp_log( "[cdda-mp] HOST-SUMMON: REJECTED, out of bubble abs=(" +
                std::to_string( abs.x() ) + "," + std::to_string( abs.y() ) + "," +
                std::to_string( abs.z() ) + ")" );
        return;
    }
    monster *placed = g->place_critter_at( mid, here.get_bub( abs ) );
    if( placed == nullptr ) {
        mp_log( "[cdda-mp] HOST-SUMMON: REJECTED, no room for " + mtype_str );
        return;
    }
    placed->friendly = friendly;
    if( turns > 0 ) {
        placed->set_summon_time( time_duration::from_turns( turns ) );
    }
    placed->no_extra_death_drops = no_drops;
    placed->no_corpse_quiet = quiet;
    // Summoner is the client's proxy NPC, so SP's own summon bookkeeping
    // (ownership, death cleanup) treats it as that character's summon -- which
    // is what it is.
    if( npc *proxy = get_partner_npc() ) {
        placed->set_summoner( proxy );
    }
    // Deliberately NOT assigning a net id here: build_monster_list() gives one
    // to any monster with mp_net_id == 0 on the next broadcast, which is the
    // single place that allocation happens.  Duplicating it here would be a
    // second allocator to keep in step.
    mp_log( "[cdda-mp] HOST-SUMMON-PLACE: " + mtype_str +
            " abs=(" + std::to_string( abs.x() ) + "," + std::to_string( abs.y() ) + "," +
            std::to_string( abs.z() ) + ")" +
            " friendly=" + std::to_string( friendly ) +
            " turns=" + std::to_string( turns ) );
}

// --- Deliberate client HP changes (ROADMAP B2) ----------------------------

namespace
{
// Arming depth rather than a bool: spell::heal() calls healall(), which calls
// heal() per part, and a future caller could nest scopes.  Only the outermost
// destructor sends.
int g_hp_scope_depth = 0;
std::string g_hp_scope_source;
std::map<std::string, int> g_hp_pending;   // bodypart id -> accumulated delta
} // namespace

mp_hp_event_scope::mp_hp_event_scope( const std::string &source )
{
    if( g_hp_scope_depth == 0 ) {
        g_hp_scope_source = source;
        g_hp_pending.clear();
    }
    ++g_hp_scope_depth;
}

mp_hp_event_scope::~mp_hp_event_scope()
{
    if( --g_hp_scope_depth > 0 ) {
        return;   // inner scope; the outermost one sends
    }
    if( !is_client_mode() || g_hp_pending.empty() ) {
        g_hp_pending.clear();
        return;
    }
    std::string parts;
    for( const auto &kv : g_hp_pending ) {
        if( kv.second == 0 ) {
            continue;
        }
        if( !parts.empty() ) {
            parts += ',';
        }
        parts += "{\"bp\":\"" + kv.first + "\",\"d\":" + std::to_string( kv.second ) + "}";
        // Move the damage-narration baseline by the same amount, so when the
        // host echoes this change back as an absolute value the client does not
        // announce its own spell cost as "You are hit for N damage!".
        mp_hp_baseline_adjust( kv.first, kv.second );
    }
    g_hp_pending.clear();
    if( parts.empty() ) {
        return;
    }
    const std::string payload = "{\"type\":\"client_hp\",\"src\":\"" + g_hp_scope_source +
                                "\",\"parts\":[" + parts + "]}";
    mp_log( "[cdda-mp] CLI-HP-EVENT: " + payload );
    client_send( payload );
}

void mp_note_hp_event( const Character &who, const bodypart_id &bp, int delta )
{
    if( g_hp_scope_depth == 0 || delta == 0 || !is_client_mode() || !who.is_avatar() ) {
        return;
    }
    g_hp_pending[bp.id().str()] += delta;
}

void mp_log_ally_target_check( const Creature &caster, const Creature &target,
                               const std::string &spell_id_str, int attitude,
                               bool accepts_ally, bool verdict )
{
    if( !is_hosting() && !is_client_mode() ) {
        return;
    }
    if( !accepts_ally ) {
        return;
    }
    const Character *tgt_ch = target.as_character();
    if( tgt_ch == nullptr || !is_partner_npc( tgt_ch->getID() ) ) {
        return;   // only the partner proxy is interesting here
    }
    static std::string s_last;
    const std::string key = spell_id_str + ":" + std::to_string( verdict );
    if( key == s_last ) {
        return;
    }
    s_last = key;
    const npc *tgt_npc = dynamic_cast<const npc *>( &target );
    mp_log( "[cdda-mp] MAGIC-ALLY-TARGET: spell=" + spell_id_str +
            " target='" + tgt_ch->name + "'" +
            " dist=" + std::to_string( rl_dist( caster.pos_abs(), target.pos_abs() ) ) +
            " attitude=" + std::to_string( attitude ) +
            " (0=hostile 1=neutral 2=friendly)" +
            " player_ally=" + std::to_string( tgt_npc != nullptr && tgt_npc->is_player_ally() ) +
            " VALID=" + std::to_string( verdict ) );
}

namespace
{
// Set while we are applying a spell that arrived from the partner, so the
// application cannot dispatch straight back and ping-pong.
bool g_applying_partner_spell = false;
} // namespace

bool mp_dispatch_spell_at_partner( const spell &sp, Creature &caster, Creature &target )
{
    if( !is_hosting() && !is_client_mode() ) {
        return false;
    }
    if( g_applying_partner_spell ) {
        return false;   // re-entrancy: we ARE the far side, apply locally
    }
    if( !caster.is_avatar() ) {
        return false;   // only our own casts can be forwarded
    }
    const Character *tgt = target.as_character();
    if( tgt == nullptr || !is_partner_npc( tgt->getID() ) ) {
        return false;
    }
    // Support only, for now -- see the note in the header.
    if( !sp.is_valid_target( spell_target::ally ) ||
        sp.is_valid_target( spell_target::hostile ) ) {
        return false;
    }
    const std::string payload = "{\"type\":\"partner_spell\",\"spell\":\"" + sp.id().str() +
                                "\",\"level\":" + std::to_string( sp.get_level() ) + "}";
    mp_log( "[cdda-mp] PARTNER-SPELL-SEND: " + payload );
    if( is_client_mode() ) {
        client_send( payload );
    } else if( server *srv = get_active_server() ) {
        srv->post_broadcast( payload + "\n" );
    }
    // The caster still sees something happen -- without this the spell reads as
    // a no-op on their own screen even though it worked.
    caster.add_msg_if_player( m_good, _( "Your magic reaches %s." ), tgt->name );
    return true;
}

void mp_handle_partner_spell( const std::string &msg )
{
    if( !is_hosting() && !is_client_mode() ) {
        return;
    }
    std::string sp_str;
    int level = 0;
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        sp_str = jo.get_string( "spell", "" );
        level = jo.get_int( "level", 0 );
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] PARTNER-SPELL parse error: " ) + e.what() );
        return;
    }
    const spell_id sid( sp_str );
    if( sp_str.empty() || !sid.is_valid() ) {
        mp_log( "[cdda-mp] PARTNER-SPELL: REJECTED, invalid spell '" + sp_str + "'" );
        return;
    }
    avatar &av = get_avatar();
    spell sp( sid );
    sp.set_level( av, level );
    // Cast on ourselves, at our own position, with the caster's level.  Runs the
    // real SP effect path rather than a reimplementation of it, so heals,
    // effect_str, DOTs and extra_effects all behave exactly as in single player.
    //
    // Known v1 limitation: an area support spell re-centres on the receiver
    // rather than preserving the original blast geometry.  Acceptable while
    // this is restricted to deliberate support -- the receiver IS the intended
    // beneficiary and any splash lands where the caster was aiming anyway.
    g_applying_partner_spell = true;
    sp.cast_all_effects( av, av.pos_bub() );
    g_applying_partner_spell = false;
    mp_log( "[cdda-mp] PARTNER-SPELL-APPLY: spell=" + sp_str +
            " level=" + std::to_string( level ) +
            " dmg=" + std::to_string( sp.damage( av ) ) );
}

void mp_handle_client_hp( const std::string &msg )
{
    if( !is_hosting() ) {
        return;
    }
    npc *proxy = get_partner_npc();
    if( proxy == nullptr ) {
        mp_log( "[cdda-mp] HOST-HP-EVENT: no proxy, dropped" );
        return;
    }
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        const std::string src = jo.get_string( "src", "?" );
        if( !jo.has_array( "parts" ) ) {
            return;
        }
        std::string applied;
        for( const JsonValue &pv : jo.get_array( "parts" ) ) {
            JsonObject po = pv.get_object();
            po.allow_omitted_members();
            const std::string bp_str = po.get_string( "bp", "" );
            const int d = po.get_int( "d", 0 );
            if( bp_str.empty() || d == 0 ) {
                continue;
            }
            const bodypart_id bp = bodypart_str_id( bp_str ).id();
            // The proxy is built from the client's real stats, so it has the
            // same parts -- but a mutation that grants one may not have synced
            // yet, and mod_part_hp_cur on a missing part debugmsgs.
            if( !bp.is_valid() || !proxy->has_part( bp ) ) {
                mp_log( "[cdda-mp] HOST-HP-EVENT: proxy lacks bp=" + bp_str + ", skipped" );
                continue;
            }
            const int before = proxy->get_part_hp_cur( bp );
            proxy->mod_part_hp_cur( bp, d );
            applied += bp_str + ":" + std::to_string( before ) + "->" +
                       std::to_string( proxy->get_part_hp_cur( bp ) ) + " ";
        }
        mp_log( "[cdda-mp] HOST-HP-EVENT: src=" + src + " [" + applied + "]" );
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] HOST-HP-EVENT parse error: " ) + e.what() );
    }
}

void mp_log_proxy_magic_state()
{
    if( !is_hosting() ) {
        return;
    }
    // Host side, so get_partner_npc() is the CLIENT's proxy.
    npc *proxy = get_partner_npc();
    if( proxy == nullptr ) {
        mp_log( "[cdda-mp] MAGIC-PROXY: no proxy NPC yet" );
        return;
    }
    std::string known;
    for( const spell_id &sp : proxy->magic->spells() ) {
        if( !known.empty() ) {
            known += ',';
        }
        known += sp.str();
    }
    // Expected result: spells=0 mana=0.  If that holds, every host-side
    // enchantment or EOC that reads the joining player's magic (Magiclysm's
    // class damage resistances are u_school_level()/6) computes as if they were
    // a mundane.  See ROADMAP B4.
    mp_log( "[cdda-mp] MAGIC-PROXY: name='" + proxy->name +
            "' spells=" + std::to_string( proxy->magic->spells().size() ) +
            " mana=" + std::to_string( proxy->magic->available_mana() ) +
            "/" + std::to_string( proxy->magic->max_mana( *proxy ) ) +
            " known=[" + ( known.empty() ? "none" : known ) + "]" );
}

} // namespace cata_mp
