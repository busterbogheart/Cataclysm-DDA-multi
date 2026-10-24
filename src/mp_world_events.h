#pragma once
#ifndef CATA_SRC_MP_WORLD_EVENTS_H
#define CATA_SRC_MP_WORLD_EVENTS_H

// Co-op: world EVENTS (explosions, blasts) as opposed to world STATE.  State
// crosses as diffs; an event happens in whichever world runs it.  This is the
// seam the event channel (ROADMAP "THE MISSING EVENT CHANNEL", 2026-08-26) would
// grow from.  For now it only reports which world ran an event.

#include <memory>
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

class mp_hp_event_scope;

// Wraps map::creature_in_field.
//
// Client: fields hurt the client's own avatar in the client's world, from the
// fields the host syncs.  The host's world never applies them to the proxy (the
// proxy is skipped in do_turn's NPC loop), and the host's HP is authoritative,
// so the next state packet undid that damage: near-immunity to fire and acid.
// Measured 2026-10-24: HP-AUTHORITY-BG 250 overwrites, cumulative |delta| 8747
// in ~3 min of standing in an incendiary's fire; the proxy lost ~17.  So the
// HP the fields take is reported to the host through the client_hp channel,
// the way a blood-magic cost is, and the host applies it to the proxy.
//
// DIAGNOSTIC, both sides: when the creature is a player in co-op (the client's
// avatar on the client; the client's proxy on the host) and the fields on its
// tile changed its HP, pain or effects, logs FIELD-HIT with side, fields with
// intensity, HP before -> after (total, torso, head), pain, effects gained/lost.
// No-op otherwise.
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
        int pain = 0;
        std::set<std::string> effects;   // "id|bp"
        std::unique_ptr<mp_hp_event_scope> hp_report;   // client avatar only
};

} // namespace cata_mp

#endif // CATA_SRC_MP_WORLD_EVENTS_H
