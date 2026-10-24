#include "mp_world_events.h"

#include <string>

#include "creature.h"
#include "mp_client_conn.h"
#include "mp_gamestate.h"

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

} // namespace cata_mp
