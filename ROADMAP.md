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
  - [NPC proxy fidelity](#npc-proxy-fidelity)
  - [MP-only scenarios](#mp-only-scenarios)
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

### NPC proxy fidelity
- EOC (Effect on Condition) not processed on proxy NPC — conditional effects, missions, morale events targeting remote player silently no-op
- NPC healing not applied to proxy — bleed/wound sync works but natural healing ticks are skipped
- Static NPCs from mapgen (`map::place_npc()`, `create_starting_npcs()`) still spawn despite scenario filter; need mapgen guards

### MP-only scenarios
- Scenario picker already filters to `LONE_START` scenarios in MP mode (`newcharacter.cpp:3081`)
- **Easy part** — new `MULTIPLAYER_ONLY` flag + one-line filter to hide those scenarios in SP:
  ```cpp
  if( !cata_mp::is_mp_mode() && scen.has_flag( "MULTIPLAYER_ONLY" ) ) continue;
  ```
- Then add JSON scenarios with tuned starting conditions for 2 players: threat density, complementary forced traits, co-op starting missions, co-op loot balance
- **Hard part** — client spawn location: currently client always teleports to host position regardless of scenario; a proper MP scenario needs the host to designate a client-specific spawn point in the join message (protocol change)
- Flag descriptions array in `newcharacter.cpp` (~line 1919) needs a new entry for `MULTIPLAYER_ONLY` to show in the UI

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
