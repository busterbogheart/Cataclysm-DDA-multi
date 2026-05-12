# Multiplayer Roadmap

Status as of 2026-05-12.

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
  - [NPC proxy fidelity](#npc-proxy-fidelity)
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
- Host-driven vehicles not broadcast to client
- Vehicle runtime spawn (debug-spawned mid-session) not visible to other player — no creation path in vehicle sync
- Passengers not handled

### NPC proxy fidelity
- EOC (Effect on Condition) not processed on proxy NPC — conditional effects, missions, morale events targeting remote player silently no-op
- NPC healing not applied to proxy — bleed/wound sync works but natural healing ticks are skipped
- Static NPCs from mapgen (`map::place_npc()`, `create_starting_npcs()`) still spawn despite scenario filter; need mapgen guards

### Headless dedicated server
- `--server` mode implemented (loads world without SDL) but not fully tested
- Periodic autosave for server mode not implemented

### Code quality
- `do_turn.cpp` inline MP blocks should be refactored into named callouts (`mp_pre_monmove()`, `mp_client_auto_wait()`, `mp_post_activity()`) — reduces merge conflict surface with upstream

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
