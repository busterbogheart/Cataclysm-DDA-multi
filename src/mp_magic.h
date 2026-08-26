#pragma once
#ifndef CATA_SRC_MP_MAGIC_H
#define CATA_SRC_MP_MAGIC_H

#include <string>

#include "type_id.h"

class Character;
class Creature;
class event_bus;
class monster;
class spell;

// Co-op diagnostics for spell content (Magiclysm, Sorcerer, Xedra, anything else
// that casts).  Magiclysm was downgraded from mod_coop::incompatible to
// mod_coop::warn on 2026-08-25 specifically so its real co-op behavior could be
// MEASURED rather than theorized — this file is that measurement.  Nothing in
// here changes game behavior; it only logs.
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

// --- Deliberate client HP changes (ROADMAP B2) ----------------------------
//
// The host's proxy HP is authoritative: apply_monster_sync's sibling, the
// bodyparts applier, does an unconditional set_part_hp_cur() with the host's
// value.  So anything the CLIENT does to its own HP is discarded on the next
// sync -- a Restoration self-heal is reverted, and an Animist HP cost is
// refunded, which is a cheat rather than a mere break.
//
// Measured 2026-08-25: that overwrite fires CONSTANTLY, 2448 times in one
// session with deltas of +1..+5, because the client's avatar and the host's
// proxy each run their own natural regen.  That measurement is why this is an
// EXPLICIT EVENT channel and not the HP-diff channel originally specced:
// natural regen goes through Character::heal() -> mod_part_hp_cur(), the very
// same chokepoint spells use, so a diff (or a blind hook) would report the
// client's regen to a host that is already computing its own and the character
// would double-heal.  Contamination is continuous, so no threshold filters it.
//
// Instead, reporting is ARMED only around genuinely deliberate changes.  Scope
// one of these around the mutation and nothing else is ever reported.
class mp_hp_event_scope
{
    public:
        explicit mp_hp_event_scope( const std::string &source );
        ~mp_hp_event_scope();
        mp_hp_event_scope( const mp_hp_event_scope & ) = delete;
        mp_hp_event_scope &operator=( const mp_hp_event_scope & ) = delete;
        mp_hp_event_scope( mp_hp_event_scope && ) = delete;
        mp_hp_event_scope &operator=( mp_hp_event_scope && ) = delete;
};

// Called from Character::mod_part_hp_cur().  No-op unless a scope is armed, we
// are the client, and `who` is our own avatar.  Accumulates per-bodypart; the
// scope's destructor sends one packet for the whole change (healall touches
// every part, and that should not be a dozen packets).
void mp_note_hp_event( const Character &who, const bodypart_id &bp, int delta );

// Diagnostic for ally-targeted spells (ROADMAP B5).  Called from
// spell::is_valid_target's creature branch.  Fires only in co-op, only when the
// spell accepts allies, and only for the partner's proxy -- otherwise every
// targeting sweep over every creature would spam.  Rate-limited to one line per
// distinct (spell, verdict) so holding the cursor still does not flood.
void mp_log_ally_target_check( const Creature &caster, const Creature &target,
                               const std::string &spell_id_str, int attitude,
                               bool accepts_ally, bool verdict );

// --- Partner-targeted spells (ROADMAP B5) --------------------------------
//
// The other player is an NPC proxy in your world -- a puppet.  A support spell
// aimed at them heals/buffs the puppet and the real person never learns of it.
// Measured 2026-08-25: druidic_healing billed the caster 15 HP
// (CLI-HP-EVENT arm_l -15) and emitted no corresponding heal event at all,
// because the thing healed was not an avatar.
//
// Returns true when it has taken responsibility for this target: the spell was
// forwarded and the caller must NOT apply it locally.  False means "not mine",
// and the normal SP path runs unchanged.
//
// v1 forwards DELIBERATE SUPPORT ONLY -- spells that accept `ally` but not
// `hostile`.  A spell that accepts both is an area attack that caught the
// partner incidentally, and forwarding it would need the whole blast geometry
// re-resolved on the far side to be correct; those keep today's behavior
// (partner untouched) until that is worth building.  Of the 124 Magiclysm
// spells accepting `ally`, 18 are support and 106 are area attacks.
bool mp_dispatch_spell_at_partner( const spell &sp, Creature &caster, Creature &target );

// Co-op guard for long casts.  SP's safe-mode warns about hostiles near YOU; this
// asks about hostiles near your PARTNER, which is the half of the shared world you
// cannot see and the half that decides whether committing to a multi-minute cast is
// suicide for the pair.  True = go ahead (the overwhelmingly common answer): outside
// MP, with no partner, for casts under MP_LONG_CAST_MOVES, or when the partner is
// clear.  False only when the player answered "no" to the prompt.
bool mp_confirm_long_cast( const spell &sp, Character &caster );

// Receiver: apply a {"type":"partner_spell",...} packet to our own avatar.
// Symmetric -- host and client both send and both receive.
void mp_handle_partner_spell( const std::string &msg );

// Host: apply a {"type":"client_hp",...} packet to the proxy.  Runs in
// handle_remote_action, which process_mp_events() drains before
// grant_client_turn() and therefore before serialize_remote_player_state() --
// so the absolute HP the host broadcasts back already includes this, and the
// round-trip is consistent instead of fighting itself.
void mp_handle_client_hp( const std::string &msg );

} // namespace cata_mp

#endif // CATA_SRC_MP_MAGIC_H
