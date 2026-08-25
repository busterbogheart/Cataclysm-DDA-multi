#include "mp_magic.h"

#include <string>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "effect.h"
#include "event.h"
#include "event_bus.h"
#include "event_subscriber.h"
#include "game.h"
#include "magic.h"
#include "mp_client_conn.h"
#include "mp_gamestate.h"
#include "npc.h"
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
