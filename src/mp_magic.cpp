#include "mp_magic.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bodypart.h"
#include "cata_scope_helpers.h"
#include "line.h"

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "effect.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "event.h"
#include "event_bus.h"
#include "event_subscriber.h"
#include "flexbuffer_json.h"
#include "game.h"
#include "json_loader.h"
#include "magic.h"
#include "magic_enchantment.h"
#include "damage.h"
#include "enum_conversions.h"
#include "map.h"
#include "monster.h"
#include "mp_client_conn.h"
#include "mp_gamestate.h"
#include "mp_server.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "uistate.h"
#include "enums.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"

static const efftype_id effect_pet( "pet" );

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
        // Un-hides event_subscriber's 3-arg notify() overload (talker-pair variant),
        // which we don't use. Without this, declaring the 1-arg override below hides
        // ALL base overloads of the name per C++ name lookup, not just the one being
        // overridden. GCC's -Woverloaded-virtual flags that (-Werror on Linux CI);
        // clang's is more lenient and let this through silently. Pre-existing since
        // this class was written -- unrelated to any change in this file today.
        using event_subscriber::notify;
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
                    // B13: tell the tile scan how far this spell could have
                    // reached, so ground it changed outside the default 10-tile
                    // box still reaches the host.  Read off the caster's OWN
                    // copy of the spell rather than the type, because range and
                    // aoe are both level-scaled — the type's maximum would
                    // over-scan by a lot at low levels.  A spell cast from an
                    // item or scroll is not in the spellbook; log that rather
                    // than silently under-scanning, since it is the case most
                    // likely to look like this fix "not working".
                    if( who->magic->knows_spell( sp ) ) {
                        const spell &cast_sp = who->magic->get_spell( sp );
                        const int reach = cast_sp.range( *who ) + cast_sp.aoe( *who );
                        line += " reach=" + std::to_string( reach );
                        mp_note_spell_tile_reach( reach );
                    } else {
                        line += " reach=unknown(not-in-spellbook)";
                    }
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

bool mp_client_summon_vehicle( const spell &sp, Creature &caster, const tripoint_bub_ms &target )
{
    if( !is_client_mode() || !caster.is_avatar() ) {
        return false;   // the host, or not our own cast: SP places it as usual
    }
    const tripoint_abs_ms abs = get_map().get_abs( target );
    const bool permanent = sp.has_flag( spell_flag::PERMANENT );
    const int turns = permanent ? 0 : to_turns<int>( sp.duration_turns( caster ) );
    const std::string payload =
        std::string( "{\"type\":\"client_summon_vehicle\",\"proto\":\"" ) +
        sp.summon_vehicle_id().str() +
        "\",\"x\":" + std::to_string( abs.x() ) +
        ",\"y\":" + std::to_string( abs.y() ) +
        ",\"z\":" + std::to_string( abs.z() ) +
        ",\"permanent\":" + ( permanent ? "true" : "false" ) +
        ",\"turns\":" + std::to_string( turns ) + "}";
    mp_log( "[cdda-mp] CLI-SUMMON-VEH-SEND: " + payload );
    client_send( payload );
    return true;
}

void mp_handle_client_summon_vehicle( const std::string &msg )
{
    if( !is_hosting() ) {
        return;
    }
    std::string proto_str;
    tripoint_abs_ms abs;
    bool permanent = false;
    int turns = 0;
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        proto_str = jo.get_string( "proto", "" );
        abs = tripoint_abs_ms{ jo.get_int( "x", 0 ), jo.get_int( "y", 0 ), jo.get_int( "z", 0 ) };
        permanent = jo.get_bool( "permanent", false );
        turns = jo.get_int( "turns", 0 );
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] HOST-SUMMON-VEH parse error: " ) + e.what() );
        return;
    }
    const vproto_id proto( proto_str );
    if( proto_str.empty() || !proto.is_valid() ) {
        mp_log( "[cdda-mp] HOST-SUMMON-VEH: REJECTED, invalid vehicle '" + proto_str + "'" );
        return;
    }
    map &here = get_map();
    if( !here.inbounds( abs ) ) {
        // The client already paid for the cast; out of the host's bubble is the
        // pair being too far apart, not a bug here (same as HOST-SUMMON).
        mp_log( "[cdda-mp] HOST-SUMMON-VEH: REJECTED, out of bubble abs=" + abs.to_string() );
        return;
    }
    const tripoint_bub_ms bub = here.get_bub( abs );
    // spawn_summoned_vehicle's own check, against the host's world.
    if( here.veh_at( bub ) ) {
        mp_log( "[cdda-mp] HOST-SUMMON-VEH: REJECTED, already a vehicle at " + abs.to_string() );
        return;
    }
    // Exactly spawn_summoned_vehicle's placement and settings, with the client's
    // proxy as the owner (the caster, in the host's world).
    vehicle *veh = here.add_vehicle( proto, bub, -90_degrees, 100, veh_spawn_status::PRISTINE,
                                     false, true );
    if( veh == nullptr ) {
        mp_log( "[cdda-mp] HOST-SUMMON-VEH: REJECTED, add_vehicle failed for " + proto_str +
                " at " + abs.to_string() );
        return;
    }
    veh->unlock();
    veh->magic = true;
    if( !permanent ) {
        veh->summon_time_limit = time_duration::from_turns( turns );
    }
    if( npc *proxy = get_partner_npc() ) {
        veh->set_owner( *proxy );
    }
    // No net id here: the vehicle broadcast assigns one to any new vehicle, the
    // same single allocator HOST-SUMMON relies on for monsters.
    mp_log( "[cdda-mp] HOST-SUMMON-VEH-PLACE: " + proto_str + " \"" + veh->name + "\" abs=" +
            abs.to_string() + ( permanent ? " permanent" : " turns=" + std::to_string( turns ) ) );
}

// A spell (or any of its extra effects, recursively) that changes world state
// the host owns: creates, revives, transforms or splits monsters, or fires a
// world event.  `depth` bounds a pathological extra_effects cycle.
static bool spell_changes_host_world( const spell_id &sp, std::string &which, int depth = 0 )
{
    static const std::set<std::string> host_owned = {
        "summon", "slime_split", "revive", "revive_dormant", "upgrade",
        "effect_on_condition", "timed_event", "guilt"
    };
    if( depth > 8 || !sp.is_valid() ) {
        return false;
    }
    if( host_owned.count( sp->effect_name ) ) {
        which = sp.str() + ":" + sp->effect_name;
        return true;
    }
    for( const fake_spell &extra : sp->additional_spells ) {
        if( spell_changes_host_world( extra.id, which, depth + 1 ) ) {
            return true;
        }
    }
    return false;
}

bool mp_skip_monster_death_effect( const monster &mon, bool eoc )
{
    if( !is_client_mode() ) {
        return false;
    }
    std::string which;
    if( eoc ) {
        which = "eoc:" + mon.type->mdeath_effect.eoc.value().str();
    } else if( !spell_changes_host_world( mon.type->mdeath_effect.sp.id, which ) ) {
        return false;
    }
    mp_log( "[cdda-mp] MON-DEATH-EFFECT-SKIP: " + mon.type->id.str() + " nid=" +
            std::to_string( mon.mp_net_id ) + " " + which +
            " — the host runs this death; its result arrives through sync" );
    return true;
}

void mp_on_monster_spell_op( monster &mon, const Creature &caster, const char *op )
{
    if( !is_client_mode() || !caster.is_avatar() ) {
        return;   // host is authoritative; monster/death-spell casts aren't ours
    }
    if( mon.mp_net_id == 0 ) {
        // A client-local monster the host has no copy of.  Nothing to apply
        // over there; the sync culls it either way.
        mp_log( "[cdda-mp] CLI-MON-OP: " + std::string( op ) + " on unsynced " +
                mon.type->id.str() + " — not sent" );
        return;
    }
    // The result, not the spell: type and friendliness after the effect ran.
    const std::string payload =
        std::string( "{\"type\":\"client_monster_op\",\"op\":\"" ) + op +
        "\",\"nid\":" + std::to_string( mon.mp_net_id ) +
        ",\"to\":\"" + mon.type->id.str() + "\"" +
        ",\"fr\":" + std::to_string( mon.friendly ) +
        ",\"pet\":" + ( mon.has_effect( effect_pet ) ? "true" : "false" ) + "}";
    mp_log( "[cdda-mp] CLI-MON-OP-SEND: " + payload );
    client_send( payload );
    mp_client_monster_op_sent( mon, std::string( op ) == "banish" );
}

void mp_on_spell_effect_applied( Creature &target, const Creature &caster,
                                 const efftype_id &eff, const time_duration &dur )
{
    if( !caster.is_avatar() || ( !is_client_mode() && !is_hosting() ) ) {
        return;
    }
    // DIAGNOSTIC (B5 friendly fire, 2026-10-15), both sides.  Support spells at
    // the partner were forwarded before reaching here; this is the rest.
    if( const Character *ch = target.as_character() ) {
        if( is_partner_npc( ch->getID() ) ) {
            mp_log( "[cdda-mp] PARTNER-SPELL-LOCAL-EFFECT: " + eff.str() + " " +
                    std::to_string( to_turns<int>( dur ) ) + "t on '" + ch->name +
                    "' (stand-in only, " + ( is_hosting() ? "host" : "client" ) + " cast)" );
        }
        // NPC phase 2 (B): a host NPC's local copy on the client is a stand-in; the
        // real NPC lives on the host, so the effect is replayed there.  The id is the
        // HOST's character id, kept on the copy as the "mp_synced_npc" value.
        if( is_client_mode() ) {
            if( const diag_value *tag = ch->maybe_get_value( "mp_synced_npc" ) ) {
                const std::string payload =
                    "{\"type\":\"client_monster_op\",\"op\":\"npc_effect\",\"npc\":" +
                    tag->str() + ",\"eff\":\"" + eff.str() + "\"" +
                    ",\"turns\":" + std::to_string( to_turns<int>( dur ) ) + "}";
                mp_log( "[cdda-mp] CLI-NPC-OP-SEND: " + payload );
                client_send( payload );
            }
        }
        return;
    }
    if( !is_client_mode() ) {
        return;   // the host's own monsters are authoritative already
    }
    const monster *mon = target.as_monster();
    if( mon == nullptr ) {
        return;
    }
    if( mon->mp_net_id == 0 ) {
        mp_log( "[cdda-mp] CLI-MON-OP: effect " + eff.str() + " on unsynced " +
                mon->type->id.str() + " — not sent" );
        return;
    }
    const std::string payload =
        "{\"type\":\"client_monster_op\",\"op\":\"effect\",\"nid\":" +
        std::to_string( mon->mp_net_id ) + ",\"eff\":\"" + eff.str() + "\"" +
        ",\"turns\":" + std::to_string( to_turns<int>( dur ) ) + "}";
    mp_log( "[cdda-mp] CLI-MON-OP-SEND: " + payload );
    client_send( payload );
    mp_client_note_op_effect( mon->mp_net_id, eff );
}

void mp_handle_client_monster_op( const std::string &msg )
{
    if( !is_hosting() ) {
        return;
    }
    std::string op;
    std::string to;
    std::string eff;
    int turns = 0;
    uint32_t nid = 0;
    int npc_id = 0;
    int fr = 0;
    bool pet = false;
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        op = jo.get_string( "op", "" );
        to = jo.get_string( "to", "" );
        nid = static_cast<uint32_t>( jo.get_int( "nid", 0 ) );
        npc_id = jo.get_int( "npc", 0 );
        fr = jo.get_int( "fr", 0 );
        pet = jo.get_bool( "pet", false );
        eff = jo.get_string( "eff", "" );
        turns = jo.get_int( "turns", 0 );
    } catch( const std::exception &e ) {
        mp_log( std::string( "[cdda-mp] HOST-MON-OP parse error: " ) + e.what() );
        return;
    }
    if( op == "npc_effect" ) {
        // NPC phase 2 (B): add_effect_to_target on the host's real NPC, the same call
        // the client's spell made on its copy.  Immunity and stacking decided here.
        npc *target = npc_id != 0 ? g->critter_by_id<npc>( character_id( npc_id ) ) : nullptr;
        const efftype_id eff_id( eff );
        if( target == nullptr || target->is_dead() || eff.empty() || !eff_id.is_valid() ) {
            mp_log( "[cdda-mp] HOST-NPC-OP: REJECTED effect npc=" + std::to_string( npc_id ) +
                    " eff='" + eff + "'" + ( target == nullptr ? " — no host NPC with this id" : "" ) );
            return;
        }
        const int had = target->has_effect( eff_id ) ? to_turns<int>( target->get_effect_dur( eff_id ) ) : 0;
        target->add_effect( eff_id, time_duration::from_turns( turns ) );
        mp_log( "[cdda-mp] HOST-NPC-OP: effect npc=" + std::to_string( npc_id ) + " \"" +
                target->get_name() + "\" " + eff + " +" + std::to_string( turns ) + "t: " +
                std::to_string( had ) + "t -> " +
                ( target->has_effect( eff_id ) ?
                  std::to_string( to_turns<int>( target->get_effect_dur( eff_id ) ) ) + "t" :
                  std::string( "NOT APPLIED (immune or blocked)" ) ) );
        return;
    }
    monster *mon = nullptr;
    for( const auto &ptr : get_creature_tracker().get_monsters_list() ) {
        if( ptr && ptr->mp_net_id == nid && !ptr->is_dead() ) {
            mon = ptr.get();
            break;
        }
    }
    if( nid == 0 || mon == nullptr ) {
        mp_log( "[cdda-mp] HOST-MON-OP: REJECTED " + op + " nid=" + std::to_string( nid ) +
                " — no live host monster with this nid" );
        return;
    }
    const std::string before = mon->type->id.str() + " fr=" + std::to_string( mon->friendly );
    if( op == "poly" ) {
        // magical_polymorph(): poly, then FRIENDLY_POLY's friendly = -1 for a
        // character caster.  Sending the resulting friendliness covers both;
        // for a plain polymorph it's the value the host already had.
        const mtype_id new_id( to );
        if( to.empty() || !new_id.is_valid() ) {
            mp_log( "[cdda-mp] HOST-MON-OP: REJECTED poly nid=" + std::to_string( nid ) +
                    ", unknown type '" + to + "'" );
            return;
        }
        if( mon->type->id != new_id ) {
            mon->poly( new_id );
        }
        mon->friendly = fr;
    } else if( op == "charm" ) {
        // charm_monster(): unset_dest, friendliness from the client's own
        // duration roll, and CHARM_PET's permanent pet effect.
        mon->unset_dest();
        mon->friendly = fr;
        if( pet && !mon->has_effect( effect_pet ) ) {
            mon->add_effect( effect_pet, 1_turns, true );
        }
    } else if( op == "effect" ) {
        // add_effect_to_target(): the same call the client's spell made, on the
        // host's monster.  Immunity and stacking are decided here.  Re-send the
        // record either way, so a refused effect leaves the client's copy too.
        const efftype_id eff_id( eff );
        if( eff.empty() || !eff_id.is_valid() ) {
            mp_log( "[cdda-mp] HOST-MON-OP: REJECTED effect nid=" + std::to_string( nid ) +
                    ", unknown effect '" + eff + "'" );
            mp_server_mon_force_resend( nid );
            return;
        }
        const int had = mon->has_effect( eff_id ) ? to_turns<int>( mon->get_effect_dur( eff_id ) ) : 0;
        mon->add_effect( eff_id, time_duration::from_turns( turns ) );
        mp_server_mon_force_resend( nid );
        mp_log( "[cdda-mp] HOST-MON-OP: effect nid=" + std::to_string( nid ) + " " +
                mon->type->id.str() + " " + eff + " +" + std::to_string( turns ) + "t: " +
                std::to_string( had ) + "t -> " +
                ( mon->has_effect( eff_id ) ?
                  std::to_string( to_turns<int>( mon->get_effect_dur( eff_id ) ) ) + "t" :
                  std::string( "NOT APPLIED (immune or blocked)" ) ) );
        return;
    } else if( op == "banish" ) {
        // banishment(): no drops, killed by the caster.  The proxy is the
        // client's character here, so the kill is credited the way SP credits
        // the caster.  The overflow HP cost was already paid on the client.
        mp_log( "[cdda-mp] HOST-MON-OP: banish nid=" + std::to_string( nid ) + " " + before );
        mon->death_drops = false;
        mon->die( &get_map(), get_partner_npc() );
        g->cleanup_dead();
        return;
    } else {
        mp_log( "[cdda-mp] HOST-MON-OP: REJECTED unknown op '" + op + "'" );
        return;
    }
    mp_log( "[cdda-mp] HOST-MON-OP: " + op + " nid=" + std::to_string( nid ) + " " + before +
            " -> " + mon->type->id.str() + " fr=" + std::to_string( mon->friendly ) );
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

mp_debug_body_edit_scope::mp_debug_body_edit_scope( const Character &who )
{
    if( !is_client_mode() || !who.is_avatar() ) {
        return;
    }
    who_ = &who;
    for( const bodypart_id &bp : who.get_all_body_parts() ) {
        hp_[bp.id().str()] = who.get_part_hp_cur( bp );
    }
    pain_ = who.get_pain();
}

mp_debug_body_edit_scope::~mp_debug_body_edit_scope()
{
    if( who_ == nullptr ) {
        return;
    }
    std::string parts;
    for( const auto &[bp_str, before] : hp_ ) {
        const bodypart_id bp = bodypart_str_id( bp_str ).id();
        if( !who_->has_part( bp ) ) {
            continue;
        }
        const int d = who_->get_part_hp_cur( bp ) - before;
        if( d == 0 ) {
            continue;
        }
        parts += std::string( parts.empty() ? "" : "," ) + "{\"bp\":\"" + bp_str +
                 "\",\"d\":" + std::to_string( d ) + "}";
        // As in mp_hp_event_scope: the host echoes this back as an absolute
        // value, which must not be narrated as a hit.
        mp_hp_baseline_adjust( bp_str, d );
    }
    const int pain_now = who_->get_pain();
    if( parts.empty() && pain_now == pain_ ) {
        return;
    }
    const std::string payload = "{\"type\":\"client_hp\",\"src\":\"debug\",\"parts\":[" + parts + "]" +
                                ( pain_now != pain_ ? ",\"pain\":" + std::to_string( pain_now ) : "" ) + "}";
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

// Host (2026-10-18): the stop-activity queries a partner's hit would have asked
// ("Ouch, something hurts!", "You were attacked by X!"), raised from the main
// loop by mp_host_resolve_pending_ui() instead of inside the packet handler.
// Measured 2026-10-18 (same-machine, host waiting with `|`): SP's on_hurt opened
// "You were attacked" INSIDE mp_handle_partner_spell, and its input wait applied
// the client's next messages inside the unfinished hit (MP-EVENTS-NESTED: the
// client's wait at 19:38:36, and at 19:40:22 a client MOVE, walking the proxy
// that is this hit's caster).  The client already defers the same queries for
// the host's hits (client_resolve_pending_ui).
std::vector<std::pair<distraction_type, std::string>> g_host_pending_hit_queries;
} // namespace

void mp_host_resolve_pending_ui()
{
    if( g_host_pending_hit_queries.empty() ) {
        return;
    }
    // Swap out first: a query that cancels the activity makes the rest moot,
    // and SP asks again on the next distraction anyway.
    std::vector<std::pair<distraction_type, std::string>> queries;
    queries.swap( g_host_pending_hit_queries );
    for( const auto &[type, text] : queries ) {
        const bool cancelled = g->cancel_activity_or_ignore_query( type, text );
        mp_log( "[cdda-mp] PARTNER-SPELL-HIT-QUERY: '" + text + "' asked from the main loop -> " +
                ( cancelled ? "activity cancelled" : "kept going" ) );
        if( cancelled ) {
            break;
        }
    }
}

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
    // DIAGNOSTIC (B3a, 2026-10-15): our spell is about to put an effect on a
    // monster.  Pair with MON-EFFECTS (both sides) to see whether it reaches the
    // host and what later syncs do to the client's copy.
    if( is_client_mode() && target.is_monster() && !sp.effect_data().empty() ) {
        const monster *mon = target.as_monster();
        mp_log( "[cdda-mp] CLI-SPELL-MON-EFFECT: " + sp.id().str() + " effect=" + sp.effect_data() +
                " turns=" + std::to_string( to_turns<int>( time_duration::from_moves(
                            sp.duration( caster ) ) ) ) +
                " on " + mon->type->id.str() + " nid=" + std::to_string( mon->mp_net_id ) );
    }
    const Character *tgt = target.as_character();
    if( tgt == nullptr || !is_partner_npc( tgt->getID() ) ) {
        return false;
    }
    // Support spells, and friendly fire from the client (see the B5 note in
    // the header): a spell that targets allies AND hostiles, hitting the host.
    // The host's own friendly fire on the client is left to land on the proxy,
    // which already reaches the client (proxy damage, effect hand-off).
    const bool support = sp.is_valid_target( spell_target::ally ) &&
                         !sp.is_valid_target( spell_target::hostile );
    const bool friendly_fire = is_client_mode() && sp.is_valid_target( spell_target::ally ) &&
                               sp.is_valid_target( spell_target::hostile );
    if( !support && !friendly_fire ) {
        // DIAGNOSTIC (B5 friendly fire, 2026-10-15): this spell lands on the
        // partner's stand-in in OUR world and nowhere else.  Host side, the
        // stage-2 hand-off then passes its effects on (PROXY-WORLD-EFFECT);
        // client side nothing reaches the host.
        mp_log( "[cdda-mp] PARTNER-SPELL-LOCAL: " + sp.id().str() + " (" +
                ( is_hosting() ? "host" : "client" ) + " cast) hit '" + tgt->name + "' — " +
                ( sp.is_valid_target( spell_target::ally ) ? "ally+hostile spell"
                  : "hostile-only spell (partner not FRIENDLY to the caster?)" ) +
                ", applied to the stand-in in this world only" );
        return false;
    }
    const std::string payload = "{\"type\":\"partner_spell\",\"spell\":\"" + sp.id().str() +
                                "\",\"level\":" + std::to_string( sp.get_level() ) +
                                ( friendly_fire ? ",\"hit\":true" : "" ) + "}";
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
    bool hit = false;
    try {
        JsonValue jv = json_loader::from_string( msg );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        sp_str = jo.get_string( "spell", "" );
        level = jo.get_int( "level", 0 );
        hit = jo.get_bool( "hit", false );
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

    // The caster is the PARTNER, not us, and that distinction is load-bearing
    // rather than bookkeeping.  spell::damage() builds its dialogue from
    // whichever Creature it is handed (magic.cpp:677), so a `math` formula like
    // druidic_healing's
    //     min_damage: (u_spell_level('druidic_healing') * -1) - 2
    // reads the level out of THAT character's spellbook.  Handing it our own
    // avatar made every level-scaled partner spell resolve at the level WE know
    // it — 0 for a spell we have never learned.  Measured on WAN 2026-08-27:
    //     PARTNER-SPELL-SEND  {"spell":"druidic_healing","level":10}
    //     PARTNER-SPELL-APPLY  spell=druidic_healing level=10 dmg=-2
    // -2 is exactly the level-0 minimum; level 10 should be -12..-25.  The
    // caster paid the full 35 HP and the receiver got 2 HP back.
    //
    // sp.set_level() does NOT fix this.  That sets the spell OBJECT's level,
    // which only feeds the damage_increment term of min_leveled_damage(), and
    // druidic_healing declares no damage_increment — all of its scaling lives
    // inside the math formula.  The level has to be visible on the caster.
    npc *proxy = get_partner_npc();
    Character &caster = proxy ? static_cast<Character &>( *proxy )
                        : static_cast<Character &>( av );
    if( proxy ) {
        // Mirror the caster's level onto the proxy, the same way the fork
        // already mirrors str/dex/int/per, skills and proficiencies so that
        // host-side SP handlers see real values instead of the proxy's blanks.
        // Only write when it actually differs: set_spell_level() emits
        // character_learns_spell the first time it adds an unknown spell, and
        // there is no reason to re-fire that on every cast.
        if( !proxy->magic->knows_spell( sid ) ||
            proxy->magic->get_spell( sid ).get_level() != level ) {
            proxy->magic->set_spell_level( sid, level, proxy );
        }
    }
    sp.set_level( caster, level );
    // Cast at our own position, with the caster's level.  Runs the real SP
    // effect path rather than a reimplementation of it, so heals, effect_str,
    // DOTs and extra_effects all behave exactly as in single player.
    //
    // Known v1 limitation: an area support spell re-centres on the receiver
    // rather than preserving the original blast geometry.  Acceptable while
    // this is restricted to deliberate support -- the receiver IS the intended
    // beneficiary and any splash lands where the caster was aiming anyway.
    const int hp_before = av.get_hp();
    const int pain_before = av.get_pain();
    g_applying_partner_spell = true;
    if( hit ) {
        // Friendly fire: only this tile's share, resolved against us -- our
        // dodge, armor and resistances, as SP resolves a hit on an NPC ally.
        // Not cast_all_effects, which would re-blast an area spell around us
        // and hit the monsters the caster already hit.
        //
        // With the pain and attack distractions off for the hit, so SP's
        // react_to_felt_pain / on_hurt can't open their query in here (see
        // g_host_pending_hit_queries); the rest of on_hurt (waking, adrenaline)
        // still runs.  Restored before anything else can read them.
        const bool ask_pain = uistate.distraction_pain;
        const bool ask_attack = uistate.distraction_attack;
        const bool busy = av.activity || av.has_distant_destination();
        {
            restore_on_out_of_scope<bool> restore_pain( uistate.distraction_pain );
            restore_on_out_of_scope<bool> restore_attack( uistate.distraction_attack );
            uistate.distraction_pain = false;
            uistate.distraction_attack = false;
            mp_spell_hit_single_target( sp, caster, av.pos_bub() );
        }
        // The queries SP would have asked, in its order (pain inside the damage,
        // then on_hurt).  Only if we were busy: SP's query asks nothing without
        // an activity, and a later one must not be stopped by this hit.
        if( busy && is_hosting() ) {
            if( ask_pain && av.get_pain() - pain_before >= 2 ) {
                g_host_pending_hit_queries.emplace_back( distraction_type::pain,
                        _( "Ouch, something hurts!" ) );
            }
            if( ask_attack && av.get_hp() < hp_before &&
                !av.has_effect( efftype_id( "narcosis" ) ) ) {
                g_host_pending_hit_queries.emplace_back( distraction_type::attacked,
                        av.sees( get_map(), caster )
                        ? string_format( _( "You were attacked by %s!" ), caster.disp_name() )
                        : std::string( _( "You were attacked by something you can't see!" ) ) );
            }
        }
    } else {
        sp.cast_all_effects( caster, av.pos_bub() );
    }
    g_applying_partner_spell = false;
    if( hit ) {
        mp_log( "[cdda-mp] PARTNER-SPELL-HIT: spell=" + sp_str + " level=" + std::to_string( level ) +
                " hp " + std::to_string( hp_before ) + " -> " + std::to_string( av.get_hp() ) +
                ( sp.effect_data().empty() ? std::string() :
                  " effect " + sp.effect_data() + "=" +
                  ( av.has_effect( efftype_id( sp.effect_data() ) ) ? "yes" : "no" ) ) );
    }
    // Log the damage resolved against BOTH characters.  caster_dmg is what the
    // spell is now worth; self_dmg is what the old code would have produced.
    // While this is fresh, a log line showing the two still equal means the
    // proxy did not carry the level and the fix did not take.
    mp_log( "[cdda-mp] PARTNER-SPELL-APPLY: spell=" + sp_str +
            " level=" + std::to_string( level ) +
            " caster=" + std::string( proxy ? "proxy" : "self" ) +
            " caster_dmg=" + std::to_string( sp.damage( caster ) ) +
            " self_dmg=" + std::to_string( sp.damage( av ) ) );
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
            mp_hurt_baseline_adjust( bp, proxy->get_part_hp_cur( bp ) - before );
            applied += bp_str + ":" + std::to_string( before ) + "->" +
                       std::to_string( proxy->get_part_hp_cur( bp ) ) + " ";
        }
        // A client debug edit can set pain too (the proxy's pain is host-owned).
        if( jo.has_int( "pain" ) ) {
            const int before = proxy->get_pain();
            proxy->set_pain( jo.get_int( "pain" ) );
            applied += "pain:" + std::to_string( before ) + "->" + std::to_string( proxy->get_pain() );
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

void mp_log_enchant_sig_if_changed( const Character &c, const char *tag )
{
    const enchant_cache &ec = *c.enchantment_cache;
    std::string sig;
    for( int i = 0; i < static_cast<int>( enchant_vals::mod::NUM_MOD ); ++i ) {
        const enchant_vals::mod m = static_cast<enchant_vals::mod>( i );
        const double add = ec.get_value_add( m );
        const double mul = ec.get_value_multiply( m );
        if( add == 0.0 && mul == 0.0 ) {
            continue;
        }
        sig += io::enum_to_string<enchant_vals::mod>( m ) + ":";
        if( add != 0.0 ) {
            sig += string_format( "%+g", add );
        }
        if( mul != 0.0 ) {
            sig += string_format( "x%+g", mul );
        }
        sig += " ";
    }
    for( const damage_type &dt : damage_type::get_all() ) {
        const int armor_add = ec.get_armor_add( dt.id );
        const double armor_mul = ec.get_armor_multiply( dt.id );
        if( armor_add != 0 || armor_mul != 0.0 ) {
            sig += "armor." + dt.id.str() + ":" + string_format( "%+d", armor_add ) +
                   string_format( "x%+g", armor_mul ) + " ";
        }
    }
    static std::map<std::string, std::string> s_last;
    std::string &last = s_last[tag];
    if( sig == last ) {
        return;
    }
    last = sig;
    mp_log( std::string( "[cdda-mp] " ) + tag + ": " + ( sig.empty() ? "none" : sig ) );
}

// Casts shorter than this never prompt.  3000 moves = 30 turns at speed 100, the
// same floor fast-forward uses (FF_MIN_ACTIVITY_MOVES) and for the same reason:
// below it the partner's wait is measured in seconds, so the interruption is worth
// more than the information.  Magic missile is one turn; the rituals this is for
// are hundreds.
static constexpr int MP_LONG_CAST_MOVES = 3000;
// How far from the partner a hostile still counts.  Deliberately wider than the
// distraction diagnostic's radius: the question here is "will something reach them
// during the cast", and a 30+ turn cast gives a zombie plenty of time to close.
static constexpr int MP_LONG_CAST_DANGER_RADIUS = 30;

bool mp_spell_menu_browse_only( const Character &who )
{
    // The same test both locked-action gates in handle_action() use.
    const bool browse = ( is_client_mode() || is_hosting() ) && who.is_avatar() &&
                        who.get_moves() <= 0;
    if( browse ) {
        mp_log( "[cdda-mp] SPELL-BROWSE: spell menu opened while locked — all entries disabled" );
    }
    return browse;
}

bool mp_confirm_long_cast( const spell &sp, Character &caster )
{
    if( !is_client_mode() && !is_hosting() ) {
        return true;
    }
    if( !caster.is_avatar() ) {
        return true;
    }
    const int cast_moves = sp.casting_time( caster );
    if( cast_moves < MP_LONG_CAST_MOVES ) {
        return true;
    }
    const npc *partner = get_partner_npc();
    if( !partner ) {
        return true;
    }
    const int near_partner = mp_count_hostiles_near( partner->pos_abs(),
                             MP_LONG_CAST_DANGER_RADIUS );
    if( near_partner <= 0 ) {
        return true;
    }
    // Deliberately NOT gated on hostiles near the caster: SP's safe-mode already
    // covers that case and would double-prompt.  The novel information is the half
    // of the shared world the player cannot see.
    const int turns = cast_moves / 100;
    mp_log( "[cdda-mp] LONG-CAST-GATE: spell='" + sp.id().str() +
            "' cast_moves=" + std::to_string( cast_moves ) +
            " turns=" + std::to_string( turns ) +
            " hostiles_near_partner=" + std::to_string( near_partner ) +
            " partner='" + partner->name + "'" );
    const bool proceed = query_yn( string_format(
            n_gettext( "%1$s has %2$d hostile nearby and %3$s takes about %4$d turns.  Cast anyway?",
                       "%1$s has %2$d hostiles nearby and %3$s takes about %4$d turns.  Cast anyway?",
                       near_partner ),
            partner->get_name(), near_partner, sp.name(), turns ) );
    mp_log( std::string( "[cdda-mp] LONG-CAST-GATE: answer=" ) + ( proceed ? "cast" : "abort" ) );
    return proceed;
}

} // namespace cata_mp
