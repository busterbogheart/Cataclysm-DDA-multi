# Multiplayer Roadmap

Status as of 2026-05-18.

---

## Contents

- [Blocking: Remote join flow](#blocking-remote-join-flow-gates-two-machine-testing)
- [Near-term](#near-term-unblocked-once-two-machine-play-works)
  - [Sleep](#sleep)
  - [Long actions](#long-actions-crafting-reading-construction)
  - [Performance](#performance)
  - [Missing action syncs](#missing-action-syncs)
- [Known bugs](#known-bugs)
- [Medium-term](#medium-term)
  - [Vehicles (full sync)](#vehicles-full-sync)
  - [Co-op partner assistance & time curve](#co-op-partner-assistance--time-curve)
  - [NPC proxy fidelity](#npc-proxy-fidelity)
  - [Client-side NPC visibility & interaction (Phase 2/3)](#client-side-npc-visibility--interaction-phase-23)
  - [MP-only scenarios](#mp-only-scenarios)
  - [Co-op gameplay features](#co-op-gameplay-features)
  - [Headless dedicated server](#headless-dedicated-server)
  - [Code quality](#code-quality)
- [Long-term / Big ticket](#long-term--big-ticket)
  - [Reality bubble for distant players](#reality-bubble-for-distant-players)
  - [CI / release infrastructure](#ci--release-infrastructure)
- [Done](#done)

---

## Blocking: Remote join flow (gates two-machine testing)

The current client startup is a local scaffold — full CDDA character creation → teleport to host position — and both instances share the same filesystem. This means wait timing, sleep, and long actions cannot be accurately tested until real two-machine play works. Everything in the near-term section is blocked on this.

- Host broadcasts world seed + scenario over TCP; client skips local world gen, spawns at host-designated location
- Client machine needs no pre-existing save — world state comes from host
- Architecture rule: all data through TCP, never shared filesystem

---

## Near-term (unblocked once two-machine play works)

### Sleep
- Client cannot sleep — need to dispatch sleep action to host, freeze client input during sleep, sync time advance back to client
- Mutual consent sleep design documented but not implemented
- Accurate testing requires two-machine play (shared filesystem masks timing bugs)
- **Shared bed ritual** — when both players sleep in adjacent beds/bedrolls, treat it as a single coordinated sleep with morale bonus and synced wake-up. Sidesteps the "one player blocks the other" problem by making co-sleep the intended path. Sleep state becomes "both asleep → fast-forward together" rather than "one sleeps, one waits or roams"

### Long actions (crafting, reading, construction)
- activity_actor loop correctness over the grant/wait cycle is untested — deadlock and time desync are the risks
- Crafting is highest priority; construction and reading follow
- Proxy NPC may give phantom crafting help today; real co-op crafting assist is a future feature
- Accurate testing requires two-machine play

### Performance
- **Dirty tracking for tile sync** — currently does a full 10-tile radius scan every turn; needs per-tile dirty flags to cut bandwidth and CPU cost significantly
- **Double-fire bug** — certain actions fire twice; root cause unknown, needs log investigation

### Missing action syncs
- `ACTION_UNLOAD` — wield state changes when unloading; needs `client_resync_worn()`
- `ACTION_PICK_STYLE` — martial arts style change is local-only
- `ACTION_SELECT_FIRE_MODE` / `ACTION_SELECT_DEFAULT_AMMO` — weapon mode/ammo selection local-only
- `ACTION_BIONICS` / `ACTION_MUTATIONS` — activation world effects (fields, terrain) not synced
- `ACTION_USE` / `ACTION_USE_WIELDED` — converted to wait pattern but world effects (smoke, fire, terrain mods) not fully verified
- `ACTION_CAST_SPELL` — converted to wait pattern but spell world effects beyond direct combat not verified
- `ACTION_DROP` / `ACTION_DIR_DROP` — tile sync catches drops with a lag but no explicit dispatch

---

## Known bugs

- **Vehicle desync** — after pivot fix, driver appears ~1 tile off in both views when client drives; enhanced logging added, root cause not resolved
- **Client reverse driving** — not working
- **Vehicle startup sound** — plays a "ding" instead of engine startup; regressed, root cause unknown
- **Ack-guard deadlock** — both players can lock simultaneously; 5-second auto-recovery exists but doesn't eliminate the root cause
- **Blood trail** — client blood is approximate (proxy position only, not intermediate tiles); full trail requires `client_fields` array per action (designed, not built)
- **`ACT_WAIT` edge cases** — multi-turn wait (`W`) timing has known edge cases after the sleep-bypass fix
- **Faction camp mission popups** — `MISSION_CAMP_LEADERSHIP_CHANGE` suppressed; other faction camp missions may still fire on client

---

## Medium-term

### Vehicles (full sync)

**Foundational sync gaps:**
- Host-driven vehicles not broadcast to client (only client-driven via server authority)
- Vehicle runtime spawn (debug-spawned mid-session) not visible to other player — no creation path in vehicle sync
- Passengers not handled — boarding/disembarking flow, seat swap, operating non-driver parts from passenger seat

**Vehicle interaction menu — actions NOT yet dispatched to host:**

Driving / autopilot:
- Reverse driving (known issue; not yet investigated — math looks right, likely a propagation/visual bug)
- Smart controller settings (toggle + configure)
- Individual engine control (turn specific engines on/off in multi-engine vehicles)
- Autopilot — patrol / follow / stop modes
- Pre-collision system toggle (`precollision_on` bool on vehicle; safety feature for autopilot)

Doors / curtains / lights:
- Open/close individual doors and curtains
- Open/close ALL doors/curtains (bulk)
- Control doors/curtains menu (remote operation)
- Toggle headlights / aisle lights / dome lights / cargo lights
- Camera system on/off

Cargo / consumables:
- Get items from vehicle cargo (open trunk)
- Fill container with water (vehicle tank)
- Have a drink (water tank)
- Purify water in vehicle tank
- Activate the boiler
- Reload seed drill with seeds

Security / maintenance:
- Hotwire vehicle
- Trigger / disable / smash alarm
- Disconnect power connections between linked vehicles

Turrets:
- Set turret targeting modes
- Set turret firing modes
- Aim turrets manually / auto / individual

Bike rack & towing:
- Attach to / detach from bike rack
- Hitch / unhitch towed vehicle

Creatures / furniture / non-engine parts:
- Capture/release creature in a cage part
- Tie down or remove furniture
- Harness an animal
- Activate individual non-engine parts (planter, recharger, water purifier, kitchen, chemistry lab, welding rig, etc.)

**Architectural pattern for most of the above:** each menu item runs as local SP code on whichever side opened the menu, but state changes don't propagate. Fix shape: dispatch a specific MP action from the client (e.g. `{"action":"toggle_door","vp_idx":N}`), host runs the SP code on its vehicle and broadcasts updated state. Same model that already works for `pldrive`/`cruise`/`handbrake`/`toggle_engine`/`stop_engine`/`honk`/`control_vehicle`.

### Inventory sync + authoritative-host activities

The proxy NPC currently only mirrors worn + wielded. Activities that mutate inventory (drop, craft, read, use, eat, wear/takeoff) all run locally on each side and the proxy can't actually take items out of inventory on the host. This blocks:

- Authoritative host running `drop_activity_actor` on the proxy (SP-mirror of multi-turn drops)
- Picking up world items (no MP path today)
- Crafting (recipe components must be in the proxy's inventory)
- Item use (drinking, applying, transforming) on the proxy
- Trading between players
- Death/respawn item recovery

Design doc: [`doc/MP_INVENTORY_SYNC_DESIGN.md`](doc/MP_INVENTORY_SYNC_DESIGN.md). Recommended model is host-authoritative on the proxy NPC with client-side prediction. Migration in 8 steps, ~10–15 sessions of work; first 3 steps (stable UIDs, pickup, drop unification) deliver the highest-value chunk.

Activity-id sync (display-only) shipped 2026-05-18: HUD label + one-line partner notice. That display layer is ready to hook into real authoritative activities once inventory sync lands.

### Co-op partner assistance & time curve

Two intertwined problems that the SP code model doesn't address natively:

1. **Phantom helping** — SP's `get_crafting_helpers()` automatically counts nearby ally NPCs as helpers (giving speed bonuses and "X helps with this task…" log lines). In MP the partner NPC qualifies even when the actual human player isn't trying to help — they might be eating, browsing inventory, or AFK. The bonus is currently free for proximity.
2. **Wall-clock pacing of long activities** — SP fast-forwards activities at native speed (8h sleep finishes in ~10–30 real seconds). MP lockstep ties each turn to ~1s of network RTT, so 8h would be 8 hours real-time. Players shouldn't have to watch the screen for a long craft when both are committed to non-interactive work.

The design below treats these as one feature because they share state (who is "committed" right now) and the same interruption rules (combat / range / cancel).

#### Consent model — state machine per partner

A partner is in exactly one of:
- **Idle** — not helping, not engaged in a long task.
- **Available** — willingness flag on, idle, in range (eligible to be asked).
- **Helping** — committed to the lead player's activity; moves drain in lockstep.
- **Engaged** — running their own long activity; can't help.
- **Interrupted** — was helping, then combat / range / cancel pulled them out.

Two layers of consent:

- **Willingness flag** (per-session, off by default) — a toggle that says "if my partner bumps me and asks, I'll consider it." Without this, the "Help with task" option doesn't appear on the lead player's bump menu.
- **Active engagement** — the helper bumps into the working lead player, the existing partner-menu gets a new "Help with task" entry. Selecting it commits the helper, mirroring the existing "tap on shoulder" UX.

The lead player doesn't need to confirm; they get a "X joined to help" message. The helper had to physically bump in and pick the option — that's the explicit consent.

#### Reusing SP helper code

The SP system already handles skill curves, proficiency bonuses, mood, sleep-state gating, line-of-sight, range — all the math we don't want to rebuild. The integration point:

- `get_crafting_helpers()` ([crafting.cpp:3739](src/crafting.cpp#L3739)) returns the helper list. The current MP filter excludes the partner proxy via `is_remote_player(id)`, but that check is host-only (the symmetric `is_partner_npc(id)` covers both sides — current code uses the wrong one for the symmetric helper case, but the fix is fine even though it kills today's "phantom" bonus).
- Switch the filter to: `is_partner_npc(id) && partner_state == Helping`. The proxy joins the helper list only when actively committed. Everything downstream (speed math, "helps with this task" message, observe-and-learn skill rub-off via `activity_actor.cpp:2262`) is SP code that just runs.

#### Helper-side activity

New JSON-defined activity `ACT_HELP_PARTNER`:
- Assigned to the helper avatar when they choose "Help with task".
- Mirrors the lead player's activity duration — same `moves_left` / `moves_total`, ticks with each grant.
- Empty `do_turn()`; the activity exists to consume moves and serve as the "Helping" signal flowing through the existing `client_activity` heartbeat.
- Helper sees a progress popup matching the lead player's percentage ("Helping with constructing a vehicle 47%").
- Inherits the standard distraction system — zombie shows up → existing prompt fires → activity cancels → helper drops back to Available.
- `5` to interrupt manually, same key as any other activity.

The host watches `client_activity` for `ACT_HELP_PARTNER` and flips the proxy's helper-eligibility flag accordingly. Same mechanism that already syncs ACT_WAIT / ACT_VEHICLE / ACT_CRAFT.

#### Interruption / blocker handling

- **Helper in combat** — SP distraction prompt fires for the helper → they drop ACT_HELP_PARTNER → host sees the activity clear → proxy flag flips off → lead player gets "X stopped helping (combat)" message. Lead player's activity continues solo.
- **Helper walks out of range** — same path; the helper's activity self-cancels and the host updates state.
- **Lead player cancels their own activity** — host detects the lead activity ended; sends a one-shot signal to cancel the helper's ACT_HELP_PARTNER cleanly.
- **Lead player blocked (missing component, exhausted, etc.)** — same as cancel; lead activity ends with a failure path, helper is released. SP already handles this for the lead side.
- **Helper out of ingredients** — non-issue. SP helper code only contributes moves + skill; it doesn't draw from the helper's inventory. Lead player's inventory still gates the activity.

#### Time curve / fast-forward when both committed

When both avatars are in non-interactive activities (ACT_WAIT, ACT_HELP_PARTNER, ACT_CRAFT, ACT_READ, ACT_CONSUME, sleep, etc.) **and** neither is in combat, the host enters "burst mode":

- SRV-WAIT skips its 16ms per-iter throttle and the redraw rate-limit.
- Grants/acks fly back-to-back as fast as the network round-trip allows; both sides advance one game turn per RTT instead of waiting on a frame budget.
- Optionally batch: a grant could authorize N turns of activity at once (host computes the bound from the shorter of the two `moves_left`), client ticks N times locally and acks the batch. RTT becomes the bottleneck instead of frame pacing.
- Any interrupt — distraction prompt, manual `5`, activity-finish, monster in sight — instantly drops out of burst mode back to normal lockstep cadence.
- Visible indicator in the Co-op panel ("⏩" or color shift) so the player can see why time is flying.

Exit conditions, in priority order:
1. Either side leaves the eligible activity set (new monster, manual cancel, activity finishes)
2. Calendar advances enough that a periodic event (per-minute/hour/day) fires that requires player attention
3. Network RTT spikes (burst-mode produces no benefit if RTT is the bottleneck anyway)

The sleep bypass that already exists ([mp_gamestate.cpp:2625](src/mp_gamestate.cpp#L2625)) is a hard-coded special case of this. Generalize and remove the special case.

#### Open questions

- **Symmetric or asymmetric willingness?** Does "I'm available" automatically imply "you're available to me", or are these two independent flags? Asymmetric matches reality (one player might be in DND, the other open) but doubles the protocol surface.
- **Burst-mode time scale visible to the user?** SP shows the wait_popup percentage advancing; that already conveys "time is moving". Maybe no extra UI needed beyond the burst indicator.
- **How long can burst mode run before forcing a re-check?** Probably tied to existing calendar `once_every` events — anything that already gates SP fast-forward should gate this too.
- **Helper progress bonus to skills/morale** — SP gives helpers skill XP and modifies morale. Do those apply to the helper player or just the proxy on the host? Want to verify the existing char_stats sync propagates skill XP back to the client.
- **What if the helper's character is a different sex / faction / role with helper restrictions?** SP already gates on `is_obeying` and skill checks. Should just work but worth a test pass.

#### Sketch of concrete code changes (when this leaves planning)

- New action message `{"type":"action","action":"request_help"}` from helper-side, sent when "Help with task" is chosen from the partner bump menu.
- New activity definition in `data/json/player_activities.json` for `ACT_HELP_PARTNER` with `verb: "helping"`.
- Updated filter inside `get_crafting_helpers()` to `is_partner_npc(id) && partner_activity == "ACT_HELP_PARTNER"`.
- New willingness flag on each side (per-session bool), wired into the partner bump menu's option visibility.
- New burst-mode condition inside `wait_for_client_action()` — when both avatars qualify, skip the 16ms cap and the redraw throttle. Replace the hard-coded sleep bypass with the general predicate.
- New Co-op panel field: small icon/text when burst mode is active.

Order I'd ship:
1. Filter swap `is_remote_player` → `is_partner_npc` in the helper functions (kills phantom help today, regardless of when the rest lands).
2. ACT_HELP_PARTNER activity + bump-menu entry + willingness flag — wires up the consent + engagement model.
3. Burst-mode time curve — generalizes existing sleep bypass.

### NPC proxy fidelity
- EOC (Effect on Condition) not processed on proxy NPC — conditional effects, missions, morale events targeting remote player silently no-op
- NPC healing not applied to proxy — bleed/wound sync works but natural healing ticks are skipped
- Static NPCs from mapgen (`map::place_npc()`, `create_starting_npcs()`) still spawn despite scenario filter; need mapgen guards

### Client-side NPC visibility & interaction (Phase 2/3)

The blanket "no NPCs in MP" stance was reverted (2026-05-24) so host can chat/trade with static NPCs (refugee center etc.) normally. Client side still sees nothing where the NPCs are.

**Phase 2 — visual overlay only**
- Extend the per-turn monster broadcast to include static NPCs (`is_static_npc(npc)` predicate: has `unique_id`, has named faction, or has bound mission ownership; excludes random spawns and the proxy itself)
- Client renders received NPCs as read-only sprites with name + HP, same model as the existing monster overlay
- No interaction yet — keys on adjacent NPC do nothing useful
- Smallest piece, mostly mirrors existing monster sync code

**Phase 3 — interaction proxy over TCP**
New action types over the existing message channel, each a round-trip:
- `npc_chat_open(npc_id)` → host opens dialog tree, streams topic text + choice list back
- `npc_chat_choice(idx)` → host advances dialog, sends next state
- `npc_trade_open(npc_id)` → host opens trade window, sends inventories + prices
- `npc_trade_move(...)` / `npc_trade_accept` → mutate offer state, swap items, sync client inventory delta
- Attacks (melee/ranged) go through the existing action path; host applies damage authoritatively

**Known caveats once Phase 3 lands**
- Dialog is exclusive — only one player at a time in dialog with a given NPC ("X is busy" for the second player)
- Mission ownership: missions accepted by client bind to proxy's `character_id`, persist to `mp_player_*.json`; cross-crediting kill/collect goals between players is a separate design decision
- Faction reputation is party-wide if proxy shares host's faction (likely correct for co-op; flag if not)
- Followers belong to a faction not a player — with shared faction, hired NPCs path toward whoever's closest, may produce "wait, why is X following you" moments

### MP-only scenarios
- Scenario picker already filters to `LONE_START` scenarios in MP mode (`newcharacter.cpp:3081`)
- **Easy part** — new `MULTIPLAYER_ONLY` flag + one-line filter to hide those scenarios in SP:
  ```cpp
  if( !cata_mp::is_mp_mode() && scen.has_flag( "MULTIPLAYER_ONLY" ) ) continue;
  ```
- Then add JSON scenarios with tuned starting conditions for 2 players: threat density, complementary forced traits, co-op starting missions, co-op loot balance
- **Hard part** — client spawn location: currently client always teleports to host position regardless of scenario; a proper MP scenario needs the host to designate a client-specific spawn point in the join message (protocol change)
- Flag descriptions array in `newcharacter.cpp` (~line 1919) needs a new entry for `MULTIPLAYER_ONLY` to show in the UI

### Co-op gameplay features

Mechanics that only exist because there's a partner — building trust loops and "I can't believe that worked" moments rather than pure utility.

**Team reload (heavy weapons)**

One player wields a heavy weapon (rocket launcher, grenade launcher, autocannon), the other carries the ammo. Adjacent + partner-has-matching-ammo unlocks a fast reload (~100 moves each, simultaneous) vs the normal solo time (500-1000+ moves). Solves the real problem that heavy weapons are nearly unusable solo because of suicide-tier reload time in combat; makes them the co-op signature weapon class.

Implementation:
- New keybind / bump-menu entry `ACTION_TEAM_RELOAD` on the wielder side
- Partner-adjacency + ammo-search across partner's inventory (existing inventory search code)
- Reuse `Character::reload_ammo` path, but cost both players ~100 moves instead of weapon's full reload_time
- Wire-sync ammo transfer via existing `trade_delta` (give:[ammo], take:[])
- Bump-menu fits the existing pattern; keybind is the snappier UX during combat

Extension (v2): explicit "ammo carrier" designation — partner marks ammo in their inventory as linked to your weapon, surfaces in your HUD as "linked: 12 rounds." Same mechanic, better feedback loop.

**Async shift handoff**

Player A plays solo for hours, hands the world to Player B who plays solo for hours, hands back. World clock advances continuously under whoever's hosting; off-shift player is fully disengaged. Each shift is single-player CDDA on a shared save. Sidesteps the entire time-skip / bubble / lockstep problem by making it asynchronous.

Most pieces work today via the disconnect-and-reload path; the feature is making it an intentional, supported workflow rather than a recovery procedure.

Two implementation tiers:

*Tier 1 (shippable in ~a day):* "Hand off save" menu entry. Host triggers handoff → save zips, transfers over TCP to the connected client → both sessions end → client unzips into their save dir and can now host it. No live role swap; each handoff is a clean session boundary. Player A's character continues to exist in the world as the proxy NPC under Player B's hosting (and vice versa).

*Tier 2 (live mid-session swap):* "Switch host" mid-session. Save transfers, then the client promotes itself to host mode, opens its own server, original host becomes the new client. The character-pointer swap (proxy ↔ avatar) is the hard part — your avatar becomes the proxy NPC, the proxy NPC becomes your avatar. Worth deferring until Tier 1 proves the workflow.

Why this is interesting beyond convenience: it generates DF-succession-style narratives. "Day 4 — I came back and there's a wall half-built in the kitchen. What were you thinking?" "Day 7 — there's a corpse in the bedroom and I don't know whose it is." Async shifts are arguably the most honest answer to "two players doing different things at different times" — better than expanding the bubble or implementing async-realtime sleep.

### Headless dedicated server
- `--server` mode implemented (loads world without SDL) but not fully tested
- Periodic autosave for server mode not implemented

### Code quality
- `do_turn.cpp` inline MP blocks should be refactored into named callouts (`mp_pre_monmove()`, `mp_client_auto_wait()`, `mp_post_activity()`) — reduces merge conflict surface with upstream

### Site analytics
- GA4 pageviews on cddacoop.com (gated on `PUBLIC_GA_ID` build-time var, baked into static HTML by Astro)
- `gtag('event', 'download', ...)` on the macOS download buttons — capture click intent vs. actual download counts (already free from the GitHub releases API)
- Optional: surface live download counts on the page itself from the same `/releases` response the site already fetches

---

## Long-term / Big ticket

### Reality bubble for distant players
- Only one reality bubble exists, centered on the host
- Client far from host is unsimulated (terrain, monsters, items all frozen)
- Easy first step: bump `MAPSIZE=21` (132→168 tile radius) to buy more co-op range
- True independent bubbles require a bubble manager — significant architectural change
- Tracked upstream: CleverRaven/Cataclysm-DDA#69634

### CI / release infrastructure
- macOS Intel binary shipping via mp-release.yml ✓
- ARM Mac build not yet wired up
- Linux build not yet published as artifact (sdl3-matrix tests Linux but doesn't release)
- Windows not yet targeted for release

---

## Done

**Networking / Protocol**
- TCP server/client (`mp_server`, `mp_client_conn`, `mp_queue`) with newline-delimited JSON
- `--host` / `--client` / `--server` flags; `--port` / `--password`
- Thread-safe event queue bridging network thread → game thread
- Graceful "address already in use" error; connect/disconnect detection; max 2 players enforced

**Turn cycle**
- Grant/wait lockstep: server sends grant → client acts → "wait" → host unblocks → monmove()
- `g_client_waiting_for_ack` guard prevents double-sends
- 5-second timestamp-based auto-recovery for stale ack-guard deadlocks
- Client auto-waits after 500ms idle (unblocks host during long activities)
- Host `wait_for_client_action()` skips blocking during sleep fast-forward; `ACT_WAIT` stays in lockstep
- `ACTION_PAUSE` (`.`/`5`) sends wait and zeroes moves; `ACTION_WAIT` (`W`) dispatches per-tick

**NPC proxy**
- Remote player as NPC with `is_remote_player` flag; ring-search spawn (radius 1–4)
- Character stats (str/dex/int/per, skills, proficiencies) synced with every action
- Worn items, wielded weapon, hair, gender, skin tone synced on connect and on change
- Move mode (walk/run/crouch/prone) synced with every action
- Wall-bump is free (no AP cost)
- Stamina server-authoritative (local prediction removed); auto-walk on exhaustion
- NPC speech suppressed for remote player proxy

**Tile sync**
- Terrain, furniture, items, fields, traps (both directions; `tr_null` sentinel for removal)
- Graffiti sync both directions
- `search_surroundings()` called after new traps sync (immediate detection/visibility)
- Delta-based from empty baseline; `json_escape_str()` for safe embedding

**Monster sync**
- Full monster list broadcast each turn (position, type, tile variant, HP)
- Stable tile variant via `mp_net_id` seed (deterministic across processes)
- Cleanup pass: monsters in range not in server list killed on client
- Monster HP delta: client bundles damage hits into every action; server applies before next sync

**Combat / actions**
- 13 action types converted to wait pattern: `ACTION_USE`, `ACTION_USE_WIELDED`, `ACTION_RELOAD_*`, `ACTION_UNLOAD`, `ACTION_MEND`, `ACTION_FIRE`, `ACTION_FIRE_BURST`, `ACTION_CAST_SPELL`, `ACTION_RECAST_SPELL`, `ACTION_INSERT_ITEM`, `ACTION_UNLOAD_CONTAINER`
- Smash: `destroy=false` fix (was bypassing all strength checks); "keep smashing" autosmash continuation
- Throw, eat/drink, open/close, wear/take-off, pick up, drop, auto-pickup all dispatched
- `ACT_PICKUP` and `ACT_WEAR` forced synchronous before snapshot (prevented crash from stale `item_location`)
- Host kill/damage messages forwarded to client with name substitution

**Vehicle sync**
- Position, facing, velocity, per-part open/enabled state broadcast each turn
- Part HP and fuel state synced
- Client driving: pldrive dispatch, cruise/turn split, handbrake, cruise control
- Vehicle facing indicator on client; azimuth via `turn_dir`
- `precalc_mounts` uses correct pivot path (matches host behavior)
- Local physics prediction removed from client; all state server-driven
- **Partial-turn pldrive** (commit `8e40ae27a4`, 2026-05-18) — turning charges only the SP-formula AP cost; host sends `free:true` with remaining moves so the client can chain more driving inputs (more turns, cruise changes, pause to commit) in the same turn instead of every input ending the turn
- MP actions wired: `pldrive`, `cruise`, `handbrake`, `toggle_engine`, `stop_engine`, `honk`, `control_vehicle`

**Light / bleed**
- Client sends active light value; host injects point light at proxy position
- Host sends `host_light`; client injects at host proxy
- Per-bodypart bleed intensities synced; `effect_bleed` applied on proxy

**HUD**
- Co-op debug HUD on both panels: moves, status indicator (█ traffic light), last action, partner status
- HUD invalidated after each host action (live updates)
- Host locked-state UI: map, inventory, @, options, quicksave, safemode, and more

**World / NPC suppression**
- Random NPC spawning disabled in MP mode; world slider locked + grayed
- Scenario picker filtered to 52 `LONE_START` scenarios only
- `MISSION_CAMP_LEADERSHIP_CHANGE` popup suppressed on client

**Infrastructure**
- `mp_client_post_action()` helper collapsed 19 duplicate 7-line blocks (net −87 lines)
- `start-mp.sh` with host/client/both modes, port-scan neighbor discovery, git sync
- CI: x86_64 macOS build (Rosetta), dylib bundler, `launch.command`, ccache (warm ~5-10 min vs 40 min cold), `save:` commits skip CI, `cancel-in-progress` concurrency, SDL3 matrix PRs-only
- Upstream merged (~200 commits, 3 real conflicts)
