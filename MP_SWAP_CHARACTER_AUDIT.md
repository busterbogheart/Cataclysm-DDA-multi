# MP Audit — Debug-Menu Control-NPC / Swap-Character Subsystem

Audit of the CDDA debug menu's character-control code (`Player...` submenu)
for pieces reusable by our NPC-proxy multiplayer strategy.

Trigger: noticed the debug menu's `x Control NPC follower`, `i Import
follower`, `e Export follower`, `E Export self` entries.  Our remote-player
proxy is literally an NPC, so anything CDDA already has for swapping
control between an avatar and an NPC, or for serializing a character to
JSON, is potentially directly reusable.

---

## TL;DR

Three pieces of existing CDDA code are directly relevant:

1. **`Character::swap_character()` + `avatar::control_npc()`** — in-place
   data swap between the avatar singleton and any NPC.  *Major potential
   simplification* of our per-action stat-sync machinery.  Foundational
   payoff, not performance.

2. **`npc::export_to()` + `npc::import_and_clean()`** — full character →
   JSON → full character, byte-compatible with the save format, with a
   hand-curated cross-world reset list.  *Direct fit* for the eventual
   remote-join flow.

3. **`game::add_npc_follower()` / `remove_npc_follower()`** — trivial
   follower-list registry.  We already use it correctly; noted only to
   confirm `control_npc` operates on the same registry.

The other debug-menu entries (`EDIT_PLAYER`, `DAMAGE_SELF`, `BLEED_SELF`,
`SET_AUTOMOVE`) are not relevant.

---

## 1. `Character::swap_character` + `avatar::control_npc`

### Locations

- [`src/character.cpp:605-612`](src/character.cpp#L605-L612) —
  `swap_character`
- [`src/avatar.cpp:156-191`](src/avatar.cpp#L156-L191) — `control_npc`
- [`src/avatar.cpp:1915-1924`](src/avatar.cpp#L1915-L1924) —
  `get_shadow_npc` (the swap sentinel slot the avatar owns)
- [`src/avatar.cpp:193-214`](src/avatar.cpp#L193-L214) —
  `control_npc_menu` (debug-menu wrapper)

### What it does

`swap_character` is three lines — a 3-way move via a temp `npc`:

```cpp
void Character::swap_character( Character &other )
{
    npc tmp_npc;
    Character &tmp = tmp_npc;
    tmp = std::move( other );
    other = std::move( *this );
    *this = std::move( tmp );
}
```

It works because `Character` has a real move-assignment operator that
moves every field.

`control_npc` wraps it with the bookkeeping needed to make the swap
externally consistent:

- Retarget all missions from the old avatar id to the new id.
- Swap the avatar's data into the persistent "shadow npc" slot.
- Swap the target npc with the shadow npc.
- Swap shadow-npc data back into the avatar slot.
- Update follower lists (`remove_npc_follower(old_id)`,
  `add_npc_follower(new_id)`).
- Reassign faction to `faction_your_followers`.
- Reset lightmap cache for every z-level.
- Recenter the map on the new avatar position.
- Emit `game_avatar_new` event.

After it runs, `get_avatar()` returns *the same singleton object*, but
its internal state IS the target NPC's data.  The previous avatar's
data now lives in the npc.

### Why this matters to us

Today, to dispatch a remote action with the partner's real stats, we
ship a `char_stats` block in `client_enrich_action` every action:
`str`, `dex`, `int`, `per`, all skills with non-zero level, all known
proficiencies.  Plus a separate `worn_sync` mechanism for the worn
list.  On the host side we re-inject those onto the proxy NPC before
calling `pldrive`, melee, etc.

That's:

- ~3 KB/turn on the wire while the partner is active.
- Several hundred lines of pack/unpack code across
  `mp_gamestate.cpp`.
- A **permanent maintenance liability** — every time CDDA adds a new
  stat to `Character` (a new mutation effect, a new bionic side-effect,
  a new health-related counter), we either add it to char_stats sync or
  accept that the proxy reads the host's value for that field.  Silent
  divergence by default.

With `swap_character` the shape becomes:

```cpp
get_avatar().swap_character( *proxy_npc );
run_sp_action( action );   // SP code reads get_avatar() — sees partner's data
get_avatar().swap_character( *proxy_npc );
```

The SP code doesn't know it's not the local player.  Any field CDDA
adds to Character automatically works because we're not enumerating
fields — we're swapping the whole struct.

### Caveats — why this needs a small experiment before committing

- **Local data freshness.**  Swap is local — the host's proxy npc data
  still has to be current at swap time.  We'd still sync *deltas*
  (inventory mutations, activity progress, position), just not the bulk
  read-only stats.
- **Reentrancy.**  This is the danger.  Anything running mid-swap that
  calls `get_avatar()` sees the partner's data.  UI rendering on the
  main thread could observe the wrong avatar mid-action.  Mitigation
  options: keep swap windows tiny and synchronous; or guard rendering
  against the in-swap state.
- **Doesn't help the reality-bubble problem.**  Still one avatar at a
  time, just temporarily swapped.  Long-action concurrency
  (sleep/craft) is orthogonal.
- **Move-assign cost.**  `Character` is heavy; per-action swaps aren't
  free.  Should profile before/after on the hot path.

### Estimated payoff

- Wire: ~3 KB/turn savings.  Minor relative to the 80 KB/turn we just
  killed with the baseline-sync fixes.
- Code: deletes the entire `char_stats` block in `client_enrich_action`
  (lines ~3813-3842 of `mp_gamestate.cpp`), the corresponding host-side
  apply, and most of `client_resync_worn` / `worn_sync`.
- Maintenance: removes the per-CDDA-update audit burden for new
  Character fields.

**This is a foundational win, not a performance win.**  See "Benefit
classification" at end of doc.

---

## 2. `npc::export_to` + `npc::import_and_clean`

### Locations

- [`src/savegame.cpp:2224-2231`](src/savegame.cpp#L2224-L2231) —
  `export_to`
- [`src/savegame.cpp:2154-2222`](src/savegame.cpp#L2154-L2222) —
  `import_and_clean`
- [`src/debug_menu.cpp:3569-3599`](src/debug_menu.cpp#L3569-L3599) —
  `import_folower` (the debug-menu wrapper, sic: typo in CDDA)

### What it does

`export_to(path)` writes a full character as JSON — same `serialize()`
path the save system uses.  Byte-compatible with save format.

`import_and_clean(path)` is the inverse, but with an explicit
"cross-world cleanup" pass: it deserializes, then resets fields that
shouldn't carry across worlds:

| Reset | Why |
|---|---|
| `setID(g->assign_npc_id())` | Avoid id collision in destination world |
| `last_updated`, `lifespan_end` | Timestamps from source world are meaningless |
| `effects->clear()` | Stale effects could outlive their original triggers |
| `consumption_history`, `last_sleep_check` | Source-world timestamps |
| `queued_effect_on_conditions` | Source-world refs |
| `set_pos_abs_only(defaults.pos_abs())` | Position is per-world |
| `omt_path`, `last_player_seen_pos`, `guard_pos`, `pulp_location`, `chair_pos`, `wander_pos`, `goal`, `assigned_camp` | All world-position refs |
| `known_traps.clear()`, `camps` | World-specific |
| `job` | Job assignments are world-specific |
| All mission state | `mission`, `previous_mission`, `companion_*`, `chatbin.missions*` |
| All activity state | `activity`, `backlog`, `clear_destination()`, `stashed_outbounds_*`, `activity_vehicle_part_index`, `current_activity_id` |
| `warning_record`, `complaints`, `unique_id` | World-specific social state |

### Why this matters to us

The [[architecture-rules]] memory says the current MP startup
(character creation → scenario placement → teleport to host) is a
local-only scaffold that will be replaced by a proper remote-join
flow.

When we build that flow, this is the canonical mechanism: client
exports its character to JSON via `export_to`, sends the JSON over the
wire, host runs `import_and_clean` on the proxy NPC.  Already-tested
CDDA code, well-spec'd reset list, byte-identical to the save format.

The reset list itself is *valuable independent of code reuse* — it's a
free specification of what's unsafe to copy across world boundaries.
Anything in that list is a footgun for any future migration /
join-world code we write, regardless of whether we use
`import_and_clean` itself.

### Direct uses

- **Remote join flow.**  Replace the local-only character-creation
  scaffold with a single `export_to`/`import_and_clean` round trip.
- **Character persistence across disconnects.**  Export on disconnect,
  import on reconnect with the same id reassignment.
- **Replacing piecemeal init sync.**  Today we ship worn/hair/skills/etc
  as separate post-connect messages; could collapse to one full-character
  snapshot using the same JSON format.

---

## 3. `game::add_npc_follower` / `remove_npc_follower`

### Locations

- [`src/game.cpp:1732-1740`](src/game.cpp#L1732-L1740) — the
  add/remove functions (one-liners on `u.follower_ids`)
- [`src/game.cpp:1786-1808`](src/game.cpp#L1786-L1808) —
  `validate_npc_followers` (reconciliation pass)

### What it does

Trivial setters on `u.follower_ids` (a set of `character_id`).  Used by
`control_npc` to keep the follower-list registry consistent after the
swap.

### Why this matters to us

Mostly a "no action needed, confirming we're already correct" finding.
[`src/mp_gamestate.cpp:820`](src/mp_gamestate.cpp#L820) already calls
`g->add_npc_follower(rn->getID())` to register the remote-player NPC
as a follower.  This is the same registry `control_npc` operates on,
so if we later use `swap_character` on our proxy, the follower-list
mechanics will Just Work.

---

## What I'd actually try, in order

### (a) Cheap experiment first — `import_and_clean` JSON for the join flow

Replace the current per-message init sync (worn_sync, char_stats first
broadcast, proficiency injection, etc.) with a single
`export_to`/`import_and_clean` round-trip at connect time.

- Touches the startup scaffold, not the hot path.  Low blast radius.
- Uses CDDA-tested code, gets the cross-world reset list for free.
- Byte-identical to save format, so any save-format change in upstream
  CDDA Just Works.
- Sets us up structurally for the proper remote-join flow later — the
  protocol message it produces is a permanent fixture, not throwaway.

### (b) Bigger swing later — `swap_character` per remote action

Wrap each remote-action dispatch in a `swap_character` pair, run the SP
code path against the swapped-in partner data, swap back.  Delete the
`char_stats` block, the host-side stat-injection code, and most of the
worn-sync machinery.

- Real architectural simplification (see §1).
- Needs a reentrancy audit and probably some main-thread rendering
  guard.
- Worth waiting until we have SRV-WAIT phase-timing data from the Fix-A
  re-test — if `char_stats` apply cost doesn't even register on the
  hot-path profile, the swap experiment buys us code cleanliness but
  no perf, and the timing matters for the order we tackle things.

---

## Benefit classification

The user asked: *performance, or stronger foundationally?*

**Stronger foundationally.**

- Wire savings (~3 KB/turn) are real but minor — much smaller than the
  80 KB/turn we just eliminated with the baseline-sync fixes.
- Local CPU cost of `swap_character` is non-zero and roughly cancels
  the cost saved by not re-injecting individual fields.  Net runtime
  cost is probably a wash.

The actual wins are all structural:

1. **Eliminates a class of permanent maintenance liability** — every
   new Character field CDDA adds today is one we must remember to add
   to our manual sync.  After the swap, we don't enumerate fields at
   all.

2. **Removes silent-divergence bugs** — today, when a CDDA stat we
   forgot to sync diverges between host and client, the SP code path
   reads the wrong value with no error.  With swap_character, there's
   no "forgotten" field — the whole struct is swapped or it isn't.

3. **Stays aligned with the [[client-architecture]] rule** — "mimic
   the original single-player character architecture and processes."
   `swap_character` is *the* SP mechanism for "treat this character as
   the avatar."  Using it brings us closer to that rule, not further
   from it.

4. **Sets the precedent for SP-code reuse on the proxy** — if SP
   `pldrive`/`melee`/etc. work correctly via swap, the door opens to
   running *more* SP code against the proxy without rewriting it for
   MP.

The only places performance enters the discussion at all:

- The wire savings unblock no user-visible behavior (we already fixed
  the 80 KB ping-pong).
- The local CPU cost has to be measured before we know whether it's
  meaningful — `Character` move-assign cost is unknown.

So: **foundational, not performance.**  Worth doing for code clarity
and future-proofing, not for ms saved per turn.

---

## Related memories

- [[client-architecture]] — "mimic SP character architecture" rule
- [[architecture-rules]] — startup-is-scaffold / game-loop-is-deliverable
- [[crafting-npc]] — current proxy may incidentally provide crafting
  assistance; swap_character would make that behavior explicit/intentional
