#pragma once
#ifndef CATA_SRC_MP_MAGIC_H
#define CATA_SRC_MP_MAGIC_H

#include <string>

class Creature;
class event_bus;

// Co-op diagnostics for spell content (Magiclysm, Sorcerer, Xedra, anything else
// that casts).  Magiclysm was downgraded from mod_coop::incompatible to
// mod_coop::warn on 2026-08-25 specifically so its real co-op behaviour could be
// MEASURED rather than theorized — this file is that measurement.  Nothing in
// here changes game behaviour; it only logs.
//
// Lives in an MP-only file (merge rule 1) so it can't conflict with upstream
// magic or event-bus churn.  The one SP-side touch is a single named callout in
// game::game() to register the subscriber — the event bus has no other
// registration point.
namespace cata_mp
{

// Register the co-op magic event subscriber.  Call once, next to the other
// event_bus subscribers in game::game().
void mp_subscribe_magic_events( event_bus &bus );

// True when this creature was created by a summon spell (carries a summoner).
// Used to tag monster-sync culls so "my summon vanished" reads unambiguously in
// the log instead of being indistinguishable from a local-mapgen phantom.
bool mp_is_summoned( const Creature &c );

// Turns since this instance last RESOLVED a spell, and which spell it was.
// Read by the host->client per-bodypart HP applier so an HP divergence can be
// attributed to a cast instead of to ordinary combat damage — without that
// context the two are the same signal.  Returns -1 when nothing has been cast.
int mp_turns_since_last_cast();
std::string mp_last_cast_spell();

// One-shot host-side dump of what the client's proxy NPC knows about magic.
// The proxy is assembled from char_stats + worn_sync, and neither carries
// known_magic — so this is EXPECTED to print zero spells and zero mana.  That
// is the root fact behind every host-side u_spell_level()/u_school_level()
// evaluating to 0 for the joining player (ROADMAP B4); log it rather than
// asserting it.
void mp_log_proxy_magic_state();

} // namespace cata_mp

#endif // CATA_SRC_MP_MAGIC_H
