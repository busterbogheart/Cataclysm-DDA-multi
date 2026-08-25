#pragma once
#ifndef CATA_SRC_MP_MAGIC_H
#define CATA_SRC_MP_MAGIC_H

#include <string>

class Creature;
class event_bus;
class monster;

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

// --- Client summon registration (ROADMAP B1) ------------------------------
//
// Measured 2026-08-25: a spell-summoned monster on the client is placed with
// mp_net_id == 0, and apply_monster_sync's in-region cull removes it on the
// very next sync -- 42ms after the cast, with the mana and material components
// already spent.  Net ids are only ever assigned host-side, so a client summon
// can never survive on its own.
//
// Fix is host authority, matching how the fork already handles vehicles and
// melee: the client does NOT keep its local copy.  It tells the host what to
// place and deletes its own, the host places the real monster and assigns the
// net id, and the ordinary monster broadcast delivers it back a tick later as a
// normal shared monster.  Costs one round-trip (~40ms local, ~100ms WAN)
// against a multi-thousand-move cast, and needs no new sync machinery or cull
// exemption afterwards.
//
// Called from add_summoned_mon() (magic_spell_effect.cpp), which every
// Magiclysm summon funnels through -- EOC_SUMMON_ZOMBIE and friends are only
// level-based selectors that re-cast a sub-spell with a plain `summon` effect.
// `summon_turns` is 0 for a PERMANENT summon.  No-op unless we are the client
// and the caster is our own avatar.
void mp_on_summon_placed( monster &mon, int summon_turns, bool permanent );

// Host: apply a {"type":"client_summon",...} packet.
void mp_handle_client_summon( const std::string &msg );

} // namespace cata_mp

#endif // CATA_SRC_MP_MAGIC_H
