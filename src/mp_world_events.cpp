#include "mp_world_events.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "avatar.h"
#include "bodypart.h"
#include "character.h"
#include "creature.h"
#include "effect.h"
#include "field.h"
#include "field_type.h"
#include "map.h"
#include "mp_client_conn.h"
#include "mp_gamestate.h"
#include "mp_magic.h"

namespace cata_mp
{

void mp_note_explosion( const char *kind, const tripoint_abs_ms &at, const Creature *source,
                        float power )
{
    const bool client = is_client_mode();
    if( !client && !is_hosting() ) {
        return;
    }
    mp_log( std::string( "[cdda-mp] EXPLOSION: side=" ) + ( client ? "client" : "host" ) +
            " kind=" + kind +
            " at=" + std::to_string( at.x() ) + "," + std::to_string( at.y() ) + "," +
            std::to_string( at.z() ) +
            " source=" + ( source ? source->disp_name() : std::string( "none" ) ) +
            " power=" + std::to_string( static_cast<int>( power ) ) +
            ( client ? " (the CLIENT's world ran this)" : "" ) );
}

// The effects on a creature as a sorted "id|bp" set, for a before/after diff.
static std::set<std::string> mp_effect_keys( const Creature &c )
{
    std::set<std::string> keys;
    for( const effect &e : c.get_effects() ) {
        keys.insert( e.get_id().str() + "|" + e.get_bp().id().str() );
    }
    return keys;
}

static int mp_part_hp( const Character &ch, const char *bp )
{
    return ch.get_part_hp_cur( bodypart_str_id( bp ).id() );
}

mp_field_contact_scope::mp_field_contact_scope( Creature &critter )
{
    const Character *ch = critter.as_character();
    if( ch == nullptr ) {
        return;
    }
    const bool tracked = ( is_client_mode() && ch->is_avatar() ) ||
                         ( is_hosting() && is_remote_player( ch->getID() ) );
    if( !tracked ) {
        return;
    }
    who = &critter;
    hp_total = ch->get_hp();
    hp_torso = mp_part_hp( *ch, "torso" );
    hp_head = mp_part_hp( *ch, "head" );
    pain = ch->get_pain();
    effects = mp_effect_keys( critter );
    if( is_client_mode() ) {
        hp_report = std::make_unique<mp_hp_event_scope>( "field" );
    }
}

mp_field_contact_scope::~mp_field_contact_scope()
{
    if( who == nullptr ) {
        return;
    }
    const Character *ch = who->as_character();
    if( ch == nullptr ) {
        return;
    }
    const int total_after = ch->get_hp();
    const int torso_after = mp_part_hp( *ch, "torso" );
    const int head_after = mp_part_hp( *ch, "head" );
    const int pain_after = ch->get_pain();
    const std::set<std::string> now = mp_effect_keys( *who );
    if( total_after == hp_total && pain_after == pain && now == effects ) {
        return;   // the fields here did nothing to this player this turn
    }
    std::string gained;
    std::string lost;
    for( const std::string &k : now ) {
        if( !effects.count( k ) ) {
            gained += ( gained.empty() ? "" : " " ) + k;
        }
    }
    for( const std::string &k : effects ) {
        if( !now.count( k ) ) {
            lost += ( lost.empty() ? "" : " " ) + k;
        }
    }
    std::string fields;
    for( const auto &fe : get_map().field_at( who->pos_bub() ) ) {
        fields += ( fields.empty() ? "" : " " ) + fe.first.id().str() + ":" +
                  std::to_string( fe.second.get_field_intensity() );
    }
    const tripoint_abs_ms at = who->pos_abs();
    mp_log( std::string( "[cdda-mp] FIELD-HIT: side=" ) + ( is_client_mode() ? "client" : "host" ) +
            " who='" + ch->get_name() + "' at=" + std::to_string( at.x() ) + "," +
            std::to_string( at.y() ) + "," + std::to_string( at.z() ) +
            " fields=[" + fields + "]" +
            " hp=" + std::to_string( hp_total ) + "->" + std::to_string( total_after ) +
            " torso=" + std::to_string( hp_torso ) + "->" + std::to_string( torso_after ) +
            " head=" + std::to_string( hp_head ) + "->" + std::to_string( head_after ) +
            " pain=" + std::to_string( pain ) + "->" + std::to_string( pain_after ) +
            " +[" + gained + "] -[" + lost + "]" +
            ( hp_report ? " (HP change reported to the host)" : "" ) );
}

} // namespace cata_mp
