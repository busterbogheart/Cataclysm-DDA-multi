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
};

// There is deliberately no `wait` kind.  A pause (numpad 5) telegraph was
// prototyped as a thin outline on the partner's own tile and rejected on sight
// 2026-08-25: a box around a character reads as a selection/target marker, not
// as "holding", and it fires constantly because pause is the key you mash while
// thinking.  A pause key now simply clears any staged intent, which is honest --
// they are no longer thinking about stepping anywhere.

// Sender.  Call on every action that reaches a lock gate.  Stages `act` when it
// is a movement direction, we are locked, and the step is plausible; clears the
// staged intent otherwise.  Last press wins -- a single slot, not a queue, so
// west-then-east shows east.
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
// `view_z` is the z-level currently being drawn; a hint on another level is
// dropped (and logged) here rather than silently in the renderer.
intent_kind mp_partner_intent( int view_z, tripoint_bub_ms &hint_pos, point &dir );

// The eight-style A/B scaffold (mp_intent_style_id) is gone as of 2026-08-25.
// The pick was the thin open caret with no base bar -- what the SW slot drew --
// and every direction now uses it.  See cata_tiles.cpp.

} // namespace cata_mp

#endif // CATA_SRC_MP_INTENT_H
