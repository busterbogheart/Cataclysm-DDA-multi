#pragma once
#ifndef CATA_SRC_MP_WORLD_EVENTS_H
#define CATA_SRC_MP_WORLD_EVENTS_H

// Co-op: world EVENTS (explosions, blasts) as opposed to world STATE.  State
// crosses as diffs; an event happens in whichever world runs it.  This is the
// seam the event channel (ROADMAP "THE MISSING EVENT CHANNEL", 2026-08-26) would
// grow from.  For now it only reports which world ran an event.

#include <set>
#include <string>

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

// DIAGNOSTIC (2026-10-24, fire in both worlds).  Wraps map::creature_in_field.
// When the creature is a player in co-op (the client's own avatar on the client;
// on the host, the client's proxy) and the fields on its tile changed its HP or
// effects, logs FIELD-HIT: side, fields with intensity, HP before -> after
// (total, torso, head), effects gained/lost.  Paired lines from both logs show
// whether each world applies the same field to the same player.  No-op otherwise.
class mp_field_contact_scope
{
    public:
        explicit mp_field_contact_scope( Creature &critter );
        ~mp_field_contact_scope();
        mp_field_contact_scope( const mp_field_contact_scope & ) = delete;
        mp_field_contact_scope &operator=( const mp_field_contact_scope & ) = delete;
    private:
        Creature *who = nullptr;   // null when not a tracked player
        int hp_total = 0;
        int hp_torso = 0;
        int hp_head = 0;
        std::set<std::string> effects;   // "id|bp"
};

} // namespace cata_mp

#endif // CATA_SRC_MP_WORLD_EVENTS_H
