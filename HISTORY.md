# Cataclysm: DDA Multiplayer — Session History

## Table of Contents

1. [Session 7: Eating, Wall Bump Fix, Performance Audit & Project Tracking (May 3, 2026)](#session-7)
1. [Session 6: Appearance Sync, Calendar Fix, AP Drain Fix & Performance Audit (May 3, 2026)](#session-6)
1. [Session 5: HUD Refresh Fix, Client Turn Handling & Diagnostics (May 3, 2026)](#session-5)
1. [Session 4: AP Queue & MP Debug HUD (May 1, 2026)](#session-4)
1. [Session 3: HP Sync, Inventory Sync & Testing Setup (Apr 30, 2026)](#session-3)
1. [Session 2: Client Mode, Tile Sync & Full Protocol (Phases 3–6) (Apr 30, 2026)](#session-2)
1. [Session 1: Foundation — TCP Server & NPC Wrapper (Phases 1–2) (Apr 30, 2026)](#session-1)

---

<a id="session-7"></a>
## Session 7: Eating, Wall Bump Fix, Performance Audit & Project Tracking (May 3, 2026)

### Eating — `ACTION_EAT`

Client-authoritative nutrition. The full CDDA eat pipeline runs locally on the client — inventory dialog, `eat_or_use()`, `consume_activity_actor` — so hunger, thirst, morale, addiction, and multi-turn eating all work exactly like single-player. The server just drains one AP and rebroadcasts state.

**Client side (`handle_action.cpp`):** `ACTION_EAT` is now intercepted in client mode. `game_menus::inv::consume()` dialog runs locally; if an item is selected, type is captured before `eat_or_use()` potentially invalidates the location, then `mp_dispatch` sends `{"action":"eat","item":"..."}`. Grazer/ruminant terrain-eating also handled (sends eat with empty item). Cancelled dialog → no dispatch, no AP cost.

**Server side (`mp_gamestate.cpp`):** `"action":"eat"` handler drains `remote->get_speed()` AP and broadcasts updated state. No nutritional tracking on the server — the remote NPC proxy is a puppet, not a full simulation of the client character.

### Wall Bump — Free Action (Three-Layer Fix)

Moving into an impassable tile (wall, solid furniture, etc.) no longer consumes action points.

**Layer 1 — Client pre-check (`handle_action.cpp`):** Before dispatching any movement, uses `map::open_door(check_only=true)` as a dry run. If the tile is impassable, has no creature to attack, and can't be opened — returns `false` immediately. No round-trip, no AP consumed, no server message sent. Handles ~99% of wall bumps. Doors still work: `check_only` returns `true` for closed doors, so bump-to-open dispatch still goes through.

**Layer 2 — Server `acted` flag (`mp_gamestate.cpp`):** Movement handler now tracks `bool acted`. Set to `true` only when the NPC actually moved, attacked, or opened a door. If `acted` is false at the end (wall bump that slipped past the client pre-check due to stale map state), `g_client_acted_this_turn` and `g_remote_moves = 0` are skipped, and the broadcast includes `"free":true`.

**Layer 3 — Client ack-guard (`mp_gamestate.cpp`):** On receiving `"free":true` in a state message, `g_client_waiting_for_ack` is cleared before the `moves` field is processed. This lets the server's unchanged positive `moves` value through the ack guard so the client immediately gets its moves restored.

### Performance Audit — `performance.md`

Full per-turn performance audit of all MP code paths. Nine issues ranked by severity. Two CRITICAL identified:
- **#1** (open): `g_tile_baseline` grows unbounded — no eviction, leaks per tile ever scanned
- **#2** (fixed): Host worn JSON rebuilt from scratch every broadcast

**Fix #2 applied:** Static `g_host_worn_json_cache` / `g_host_worn_sig_cache` added to `serialize_remote_player_state()`. Signature built from worn type IDs + skin_tone + hair_trait each tick. JSON string only rebuilt when signature changes. `mp_log` moved inside cache-miss branch. Skin_tone and hair computation moved before the worn block to feed the cache key.

### Project Tracking — `HISTORY.md`

`HISTORY.md` created following the same session-log format used in the supercollector project. Table of contents at top (reverse chronological), each session with `<a id>` anchor, `###` sub-headings per feature, Files Changed footer.

### Files Changed
- `src/handle_action.cpp` — `ACTION_EAT` client mode handler; movement pre-check using `open_door(check_only=true)` to skip wall bump dispatch
- `src/mp_gamestate.cpp` — `"eat"` server handler; `acted` flag in movement block; `"free":true` broadcast on no-op; `"free":true` ack-guard clear in `apply_one_state_message`; host worn JSON caching; skin_tone/hair computation moved before worn block
- `performance.md` — new file, full audit
- `HISTORY.md` — new file, session log

---

<a id="session-6"></a>
## Session 6: Appearance Sync, Calendar Fix, AP Drain Fix & Performance Audit (May 3, 2026)

### Character Appearance Sync — Move Mode Overlays

**Client → server (client's move mode):**
- `mp_dispatch` lambda in `handle_action.cpp` now appends `"move_mode"` to every outgoing action packet
- Server reads the field in `handle_remote_action()` and applies it to the remote NPC proxy immediately, before executing the move
- Tileset overlays (`overlay_run`, `overlay_crouch`, `overlay_prone`) now reflect the remote player's actual stance

**Server → client (host's move mode):**
- `serialize_remote_player_state()` includes `"host_move_mode"` every broadcast
- Client applies it to the host NPC proxy every tick (not signature-gated, since it changes per-action)

### Character Appearance Sync — Hair

**Investigation:** Server log showed `host_hair=/` — host character Karole has no `hair_style` mutation (hair is the base default, not a tagged trait). Not a sync bug; confirmed the code path is correct for characters with explicit hair mutations. Trait lookup iterates `get_mutations_variants()` filtering by `hair_style` type.

### Calendar Turn Divergence Fix

**Problem:** Host and client showed different in-game time and weather. Each character's save file stores its own `calendar::turn`; the client was using the time from their own character save instead of the shared world clock.

**Fix:** `serialize_remote_player_state()` now includes `"calendar_turn"` (integer seconds since epoch). Client applies it on every state message via `calendar::turn = time_point(N)` — same pattern used by savegame.cpp on load.

### AP Drain Fix — Info Keys (`@`, `x`, look, etc.)

**Problem:** The `@` (character sheet) and `x` (look) keys drained the client's action points via wall-clock elapsed time. `mod_moves(-current_turn.moves_elapsed())` was charging AP for all the time spent in the menu.

**Fix:** `handle_action()` now guards the `mod_moves` call behind `!cata_mp::is_client_mode()`. Client moves are server-managed; wall-clock time is irrelevant. `ACTION_TIMEOUT` was already excluded; client mode is the new second guard.

### Performance Audit — `performance.md`

Full audit of every MP code path that runs per-turn. Nine issues identified and ranked. Two CRITICAL:

**#1 (open):** `g_tile_baseline` grows unbounded — no eviction logic, leaks entries for every tile ever scanned.

**#2 (fixed this session):** Host worn JSON rebuilt from scratch every broadcast even though the host's clothing rarely changes. Fixed by caching `host_worn_json` and a signature string; JSON only rebuilt when the signature (worn type IDs + skin_tone + hair_trait) changes. The `mp_log` call also moved inside the cache-miss branch so it only fires on real changes.

**Skin tone / hair computation** moved above the worn block so they feed the cache key.

### Files Changed
- `src/handle_action.cpp` — `mp_dispatch` appends `move_mode`; `mod_moves` guarded by `!is_client_mode()`
- `src/mp_gamestate.cpp` — `handle_remote_action` reads + applies client `move_mode`; `serialize_remote_player_state` includes `calendar_turn` + `host_move_mode`; host worn JSON caching with static sig cache; `client_process_incoming` applies `calendar_turn` and `host_move_mode`
- `performance.md` — new file, full perf audit

---

<a id="session-5"></a>
## Session 5: HUD Refresh Fix, Client Turn Handling & Diagnostics (May 3, 2026)

### MP Debug HUD — Refresh Fix

HUD was not updating during `wait_for_client_action()`. Added `ensure_mp_hud()` + `ui_manager::redraw()` + `refresh_display()` inside the wait loop so the host sees live turn state while blocked on the client.

### Client Turn Handling — Lockstep Correctness

**Stale pre-ack grant bug:** After the client sends an action, the server broadcasts a `moves=0` packet (ack). But TCP buffering meant old `moves>0` grants already in the pipe would re-unlock the client before the server had processed the action — causing double-actions.

**Fix:** `g_client_waiting_for_ack` flag set immediately after `client_send()`; incoming `moves>0` packets ignored while the flag is set. Cleared when server sends `moves<=0`. 5-second safety timeout clears a stuck flag after reconnect.

### SDL Event Drain During Wait

Added an `SDL_PollEvent` loop inside `wait_for_client_action()` to handle zoom keys (`z` / `Shift+z`) live while the host waits, and to discard accumulated keypresses that would otherwise replay as unintended actions.

### Diagnostic Logging

Added structured `[cdda-mp]` log lines throughout the state processing pipeline (parsing → teleport → host NPC update → monster sync → tile sync → state applied). Logs go to `/tmp/cdda-mp-server.log` and `/tmp/cdda-mp-client.log` via `mp_log()`.

### Files Changed
- `src/mp_gamestate.cpp` — HUD refresh in wait loop; ack guard logic; SDL event drain; diagnostic logging throughout `client_process_incoming` and `apply_one_state_message`

---

<a id="session-4"></a>
## Session 4: AP Queue & MP Debug HUD (May 1, 2026)

### AP Queue — Locked Input Buffer

**Problem:** When the server locked the client (`moves=0`), key presses were ignored entirely. Players had to wait, see the lock clear, and re-press.

**Fix:** Client queues the most recent action JSON while locked (`g_pending_action`). When `client_process_incoming()` sees `moves > 0` arrive, it immediately fires the queued action. Latest keypress wins — pressing a different key replaces the queued one.

**`client_mark_action_sent()`:** Sets `g_client_waiting_for_ack` immediately after send so stale pre-ack grants are ignored (precursor to the full ack-guard fix in Session 5).

### MP Debug HUD

Persistent overlay in the bottom-left corner (46×6 ncurses window) showing:
- Current turn number + speed
- Host moves (green=ready, red=overdraft, yellow=zero)
- Client AP + lock state (server side) or client AP + queued action (client side)
- "You: acting / Host: waiting" status line mirrored on both ends

Built as a `ui_adaptor`-based window; survives screen resizes. `ensure_mp_hud()` is idempotent — safe to call every turn.

### Files Changed
- `src/mp_gamestate.cpp` — `client_queue_action()`, `client_mark_action_sent()`, `g_pending_action` auto-fire in `client_process_incoming()`; `mp_hud_t` struct + `ensure_mp_hud()`
- `src/do_turn.cpp` — `ensure_mp_hud()` called each turn in both host and client modes

---

<a id="session-3"></a>
## Session 3: HP Sync, Inventory Sync & Testing Setup (Apr 30, 2026)

### Client HP Sync — Per Bodypart

Server serializes all bodyparts from the remote NPC (`get_all_body_parts()`) as `{"id":"...", "hp":N, "hp_max":N}` array in each state broadcast. Client applies HP values directly to the avatar via `av.set_part_hp_cur(bp, new_hp)`. Client sidebar now shows accurate damage state.

**Combat message synthesis:** Client tracks `g_last_bodypart_hp` per bodypart. When HP drops between ticks a `"You are hit for N damage!"` message is generated locally so the client gets combat feedback without a dedicated message channel.

### Message Forwarding — Server → Client

`flush_action_msgs()` captures all game messages generated during a remote player's action (hits, misses, kills, sounds) and forwards them in the state broadcast's `"msgs"` array. NPC name is substituted with "You" so messages read naturally on the client. Host-avatar first-person "You..." messages that don't mention the remote NPC are suppressed.

### Inventory Sync (Initial)

Client receives the remote player's carried items from the server state and applies them to the avatar's inventory. Superseded by client-authoritative inventory in later sessions.

### Testing Infrastructure

- `save/Volta/` — dedicated world save for multiplayer testing (two characters, shared area)
- `start-mp.sh` — launch script with three modes: `both` (local co-op, launches host + client with a delay), `host` (server only), `client <ip>` (client only). Prompts for world and character selection, remembers last session with swap option.
- README updated with join instructions

### Files Changed
- `src/mp_gamestate.cpp` — `"bodyparts"` array in `serialize_remote_player_state()`; `flush_action_msgs()`; `g_action_msgs_pending`; `g_last_bodypart_hp` for delta messages
- `src/do_turn.cpp` — HP sync applied in `client_process_incoming()`
- `start-mp.sh` — new file
- `README.md` — multiplayer join instructions

---

<a id="session-2"></a>
## Session 2: Client Mode, Tile Sync & Full Protocol (Phases 3–6) (Apr 30, 2026)

### Phase 3 — Headless Server Mode

`--server` flag runs the game without SDL: skips all display init, suppresses avatar input, runs the game loop driven by network events. Server owns the simulation; clients are thin.

**Avatar immortality:** Host avatar set invincible in server mode so accidental NPC attacks or world hazards don't kill the simulation.

### Phase 4 — State Broadcast & Viewport

`serialize_remote_player_state()` broadcasts a JSON state packet after each remote player action containing: remote player position, host position, per-bodypart HP, monster list, tile changes, and a 41×21 ASCII viewport centered on the remote player.

### Phase 5 — CDDA Client Mode (`--client` flag)

Full second CDDA instance connects as a client:
- `--client <host:port>` + `--client-name <name>` CLI flags parsed in `main.cpp`
- `client_wait_for_initial_position()` blocks at startup until the server sends the first position, then teleports the client avatar to the host's area via `place_player_overmap()`
- All player input routed through `mp_dispatch` lambda instead of the normal game systems — sends JSON to server rather than executing locally

### Phase 6 — Tile/Item Sync

**Server side (`build_tile_changes()`):** Scans ±20 tiles around both the remote player and the host each turn. Compares terrain, furniture, items, and fields against `g_tile_baseline`. Only changed tiles are included in the broadcast.

**Client side (`apply_tile_changes()`):** Receives tile delta array, applies `ter_set` / `furn_set` / `i_clear` + item placement / field updates to the local map.

**Deferred join:** Client sends `{"type":"join","name":"..."}` on first tick after world load, not during connect, so the game state is fully initialized before the server spawns the remote NPC.

**Lockstep turn model:** `grant_client_turn()` called at top of host `do_turn()` — sets `g_remote_moves` and broadcasts. `wait_for_client_action()` called just before `monmove()` — blocks until client acts or 30s timeout.

### Files Changed
- `src/main.cpp` — `--server`, `--client`, `--client-name`, `--port`, `--password` flag handling
- `src/mp_gamestate.cpp` — `build_tile_changes()`, `apply_tile_changes()`, `build_monster_list()`, `apply_monster_sync()`, `client_teleport_avatar()`, `update_client_host_npc()`, `grant_client_turn()`, `wait_for_client_action()`, `client_process_incoming()`, `client_wait_for_initial_position()`
- `src/do_turn.cpp` — `grant_client_turn()` + `wait_for_client_action()` wired into turn cycle
- `src/handle_action.cpp` — `mp_dispatch` lambda; client mode routing for move/attack/open/close/smash/pickup/drop/stairs

---

<a id="session-1"></a>
## Session 1: Foundation — TCP Server & NPC Wrapper (Phases 1–2) (Apr 30, 2026)

### Phase 1 — TCP Server Infrastructure

**`src/mp_server.h/cpp`:** Standalone Asio TCP server running in a background thread. Max 2 players. JSON newline-delimited protocol. Password auth (optional). Hello/join/welcome handshake. `server::post_broadcast()` sends a message to all connected clients from any thread via strand-posted async write.

**`src/mp_queue.h/cpp`:** Thread-safe MPSC event queue bridging the network thread into the game loop. Event types: `connect`, `disconnect`, `action`. Game loop drains the queue each turn via `process_mp_events()`.

**`src/third-party/asio/`:** Asio 1.30.2 standalone headers added (no Boost dependency).

**Makefile:** Asio header path added (`-isystem src/third-party/asio`), `ASIO_STANDALONE` define, `ASIO_NO_DEPRECATED`.

**CLI flags:** `--host` (listen server in background thread, host also plays), `--server` (headless), `--port N`, `--password P`.

### Phase 2 — NPC-Wrapper Remote Player

**`src/mp_gamestate.cpp`:**
- `spawn_remote_player()` — creates a bare NPC near the host avatar using outward ring search (radius 1→20) for the nearest passable non-host tile. Loads saved character from `mp_player_<name>.json` if it exists; otherwise creates fresh. Sets ally status (`NPCATT_FOLLOW`, `your_followers` faction).
- `handle_remote_action()` — parses 8-direction JSON move commands (`{"action":"move","dir":"n"}`), calls `setpos()` with terrain-aware movement cost. Bump-to-attack on monsters; bump-to-open on doors.
- `remove_remote_player()` — saves character to disk, kills NPC, removes from overmap buffer.
- `is_remote_player()` — returns true for the remote NPC's character_id; checked by `monmove()` to skip AI processing for human-controlled NPCs.

**`src/monmove.cpp`:** `if( cata_mp::is_remote_player(npc.getID()) ) return;` — prevents AI from overriding remote player movement.

**`src/npc.cpp`:** Similar guard in NPC's own update path.

### Python Test Client

`tools/mp_client.py` — interactive terminal client for testing without a second game instance. Connects, sends join, moves with w/a/s/d/q/e/z/c, prints incoming state JSON.

### Files Changed
- `src/mp_server.h`, `src/mp_server.cpp` — new files
- `src/mp_queue.h`, `src/mp_queue.cpp` — new files
- `src/mp_gamestate.h`, `src/mp_gamestate.cpp` — new files
- `src/main.cpp` — CLI flag parsing, server thread launch
- `src/do_turn.cpp` — `process_mp_events()` called each turn
- `src/monmove.cpp`, `src/npc.cpp` — remote player guards
- `src/third-party/asio/` — Asio 1.30.2 headers
- `Makefile` — Asio include path + defines
- `tools/mp_client.py` — new file
