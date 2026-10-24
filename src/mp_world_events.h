#pragma once
#ifndef CATA_SRC_MP_WORLD_EVENTS_H
#define CATA_SRC_MP_WORLD_EVENTS_H

// Co-op: world EVENTS (explosions, blasts) as opposed to world STATE.  State
// crosses as diffs; an event happens in whichever world runs it.  This is the
// seam the event channel (ROADMAP "THE MISSING EVENT CHANNEL", 2026-08-26) would
// grow from.  For now it only reports which world ran an event.

#include "coordinates.h"

class Creature;

namespace cata_mp
{

// DIAGNOSTIC (2026-10-24, host grenade on the client).  Logs EXPLOSION with the
// side, kind and absolute position, so a host-thrown grenade that also goes off
// in the client's world shows as two lines, one in each log.  Called from
// explosion.cpp: kind "blast" (_make_explosion), "emp", "flashbang".  No-op
// outside co-op.
void mp_note_explosion( const char *kind, const tripoint_abs_ms &at, const Creature *source,
                        float power );

} // namespace cata_mp

#endif // CATA_SRC_MP_WORLD_EVENTS_H
