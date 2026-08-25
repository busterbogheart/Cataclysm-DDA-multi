#pragma once
#ifndef CATA_SRC_MP_INTENT_H
#define CATA_SRC_MP_INTENT_H

#include <string>

#include "action.h"
#include "coordinates.h"
#include "point.h"

// Intent telegraph (v1, display-only).
//
// In lockstep, whichever player is currently deliberating is the one holding a
// grant -- which means their partner is, by construction, the one blocked
// waiting on them.  The blocked player's movement keys are dropped today (see
// the DROP path in handle_action.cpp's mp_dispatch, and HOST-LOCKED-BLOCK), so
// they carry information that is simply thrown away.  Instead we stage the last
// direction pressed and ship it to the partner, who draws a faint hint tile one
// step that way.  The result is that the hint always points AT the person who
// is currently deciding, and it lasts exactly as long as their deliberation.
//
// The staged intent NEVER executes.  It is a hint, not an input buffer -- the
// player still presses the key again once they have a grant.  Making it fire on
// grant is v2 (see ROADMAP: "Intent telegraphing"), and needs a veto list
// because CDDA kills you for one accidental step.

namespace cata_mp
{

enum class intent_kind {
    none,
    move,   // stepping to an adjacent tile; `out` holds the offset
    wait,   // holding position (numpad 5); the hint lands on their own tile
};

// Sender.  Call on every action that reaches a lock gate.  Stages `act` when it
// is a movement direction or a pause, we are locked, and the step is plausible;
// clears the staged intent otherwise.  Last press wins -- a single slot, not a
// queue, so west-then-east shows east.
void mp_stage_intent_action( action_id act );

// Sender.  Drop any staged intent and tell the partner to stop drawing it.
void mp_clear_intent();

// Receiver.  Handles a {"type":"intent",...} packet.
void mp_handle_intent_recv( const std::string &msg );

// Receiver.  Returns what the partner has staged and where the hint belongs.
// Performs the lazy expiry (partner moved / aged out), the z-level check and the
// visibility gate internally, logging the reason once per intent whenever it
// decides NOT to draw -- so the renderer reduces to "draw this, here", and every
// diagnostic for this feature lives in one fork-owned file.
intent_kind mp_partner_intent( tripoint_bub_ms &hint_pos, point &dir );

// SCAFFOLDING (2026-08-24): maps a direction offset to one of eight overlay
// treatments so all eight can be compared in a single live session and one
// picked.  Collapses to a single style -- and this function disappears -- once
// the pick is made.
int mp_intent_style_id( const point &dir );

} // namespace cata_mp

#endif // CATA_SRC_MP_INTENT_H
