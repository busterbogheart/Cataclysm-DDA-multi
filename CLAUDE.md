# Cataclysm: Dark Days Ahead — Multiplayer Fork

## Project Goal

Add co-op multiplayer to Cataclysm: Dark Days Ahead (CDDA), a C++ open-world survival roguelike. The game supports both turn-based and real-time modes. This fork (`busterbogheart/Cataclysm-DDA-multiplayer`) starts from the upstream codebase with no multiplayer commits yet — this is a greenfield implementation.

---

## What Is CDDA?

An open-source post-apocalyptic survival roguelike set in procedurally generated New England. Players scavenge, craft, build/drive vehicles, and manage complex survival needs (hunger, thirst, sleep, morale, illness, mutations). One of the most feature-rich roguelikes ever made: ~251,000 lines of C++, data in hundreds of JSON files.

**Tech stack:**
- **Language:** C++17
- **Rendering:** Dual-backend — `ncurses` (ASCII) or `SDL2` (tiles)
- **Build:** GNU Makefile (primary), CMake (secondary)
- **Data:** JSON for all game content
- **Testing:** Catch2
- **Networking:** None — zero network code exists anywhere in the codebase

---

## Codebase Architecture (Key Facts for Multiplayer)

### The `game` Class — Monolithic Core
`src/game.h` / `src/game.cpp` is the center of everything:
- Holds the main game loop (`do_turn()`)
- Global avatar pointer: `get_avatar()` (was `g->u`, now being refactored away from global)
- Local map reference: `get_map()`
- Active NPC list, all global event handling

The `g->` global pattern is acknowledged as an anti-pattern; the ongoing refactor toward `get_avatar()` is useful context.

### Character Hierarchy
```
Creature (virtual base)
├── Character
│   ├── avatar   ← the player (singleton, get_avatar())
│   └── npc
└── monster
```
There is **exactly one `avatar` instance** at any time. `player` is deprecated in favor of `avatar`.

### Map / Coordinate System (5 scales)
1. **Map square (ms)** — individual tiles
2. **Submap (sm)** — 12×12 tiles; unit of save/load
3. **Overmap terrain (omt)** — 2×2 submaps (24×24 tiles); unit of map generation
4. **Overmap (om)** — 180×180 OMTs; city-scale
5. **Segment** — internal save file naming only

Coordinate types are strongly typed in `src/coordinates.h`:
- `tripoint_abs_ms` — global absolute tile position (good for network protocol; globally meaningful)
- `point_bub_ms` — bubble-relative position
- `point_abs_omt` — overmap terrain coordinate

### The Reality Bubble
A **132×132 tile (~11×11 submap)** zone centered on the avatar. Only entities inside are fully simulated. Everything outside is:
- Not simulated (terrain, items) — or
- Lightweight overmap abstraction (NPCs as walkers, monsters as `mongroup` hordes)
- Caught up on re-entry (rot, plant growth, NPC task completion)

**Critical for multiplayer:** There is only one reality bubble, centered on the single avatar. Two players far apart would require two simultaneous bubbles — currently impossible without architectural changes. GitHub issue #69634 explored partial solutions.

### The Game Loop
`do_turn()` sequence each iteration:
1. Calendar advances, weather/temperature caches clear
2. Periodic events fire (per-minute, per-hour, per-day)
3. **Player turn:** `while(avatar.moves > 0)` — **blocks waiting for player input**
4. **Monster/NPC turn:** `monmove()` processes all active creatures
5. **World updates:** vehicles, fields, weather, item decay, active items

The entire world is paused waiting for each player action. This is the core incompatibility with multiplayer.

### Long Actions / Time Skip Problem
This is the deepest challenge (acknowledged by Kevin Granade, lead dev, as the reason multiplayer "simply can not be added"):

A single player action can advance game time by:
- 1 second (single move)
- Several minutes (combat, reloading)
- **Hours** (construction, reading)
- **8+ hours** (sleeping)

**Important:** Long actions fast-forward at high speed — 8 in-game hours of sleep takes only ~10–30 seconds of wall-clock time. This makes lockstep far more viable than it first appears: other players waiting 20 seconds for a teammate to finish sleeping is acceptable.

The `activity_actor` system (`start()` / `do_turn()` / `finish()`) assumes one character, world paused. The intended multiplayer design allows players to act independently — Player A can sleep while Player B continues scavenging. This rules out pure lockstep (which requires all players to advance together) and points toward an **async real-time server model**:

- The server runs the simulation continuously on a shared clock — it never pauses waiting for player input
- Players submit actions independently and asynchronously
- Long actions (sleep, crafting, construction) are states a character enters; they resolve on their own while the world keeps ticking
- A sleeping player's character is simply passive in the shared world for ~10–30 real seconds; other players are unaffected
- Both players remain in the same timeline — no temporal desynchronization

Key implications:
- Players who are slow, AFK, or sleeping just have passive/idle characters; the world continues
- Simultaneous conflicting actions (both players grab the same item) are resolved by the server in arrival order
- The server tick rate during fast-forwarded long actions determines how quickly sleep/crafting complete in real time

### Input Pipeline — The Network Seam
```
SDL_Event / ncurses keycode
  → input_event
    → input_context (maps to abstract action string)
      → handle_action()  ← INTERCEPT POINT FOR NETWORK
        → game system (move, attack, craft, etc.)
```
The `handle_action()` boundary is the cleanest place to route remote player commands into the existing simulation — analogous to MAngband's `cmd_` / `do_cmd_` split.

### Save System
- Character state: JSON `.sav` file
- Submaps: compressed binary/JSON named by coordinate
- Overmap: per-overmap JSON files
- **No transaction semantics, no concurrent write safety, no delta tracking**

---

## Previous Multiplayer Attempts

### CataclysmLD (stolencatkarma, ~2019–2022)
Python rewrite from scratch using TCP backend. 346 commits, basic world gen, proof-of-concept connectivity. **Never shipped.** Core simulation problem ("impossible for players to be nearby each other") was never solved. Starting from scratch meant losing 15+ years of content and balance. Dormant.

### Decesus World Server (2013)
SSH-based shared world server. Players logged in via SSH to a shared save. **Failed:** 2–3 minute input lag, "last save wins" file corruption, no concurrent write protection. Abandoned at "version .001."

### Dropbox File-Sharing (Community Workaround)
Players divide the map into zones and share saves via Dropbox. Reality bubble overlaps cause data corruption. No real synchronization. Widely documented but fundamentally broken.

### WatchCDDA
SSH spectator server — multiple people watch or co-control one game instance. Not true multiplayer; streaming terminal state only.

### GitHub Issue #55749 (2022) — Time-Travel Multiplayer
Async "timeline desynchronization" where players operate at different timeline points. **Closed: not planned.**

### GitHub Issue #81834 (2023) — Hot Seat Multiplayer
Local hot-seat where players alternate; inactive players become NPCs. **Closed: stale/not planned.**

### Official CDDA Position (`doc/FREQUENTLY_MADE_SUGGESTIONS.md`)
> "This has come up many times, and it simply can not be added to DDA. The game loop of DDA includes a large number of activities that pass a large amount of time with no or minimal player input... The synchronization issues go much deeper than you seem to think they do, and resolving them would require overhauling most of the core game code."
> — Kevin Granade, lead developer

---

## How Other Roguelikes Did It

| Project | Approach | Lesson |
|---|---|---|
| NetHack / dgamelaunch | Shared server, independent sessions + spectators | Not real co-op; low effort |
| DCSS WebTiles | Same as NetHack + web streaming | Best "hosted roguelike" UX; no co-op |
| **MAngband** | **Turn-based Angband → real-time server/client** | **Most instructive precedent** |
| TomeNET / PWMAngband | MAngband derivatives | TCP mandatory; single char/account essential |
| Caves of Qud | No multiplayer shipped despite community demand | Mirrors CDDA's challenges exactly |

### MAngband (Key Lessons)
The most technically instructive precedent — Angband converted to real-time multiplayer in 1997:
- Made time flow continuously regardless of player input (energy/speed system unchanged)
- **Split `cmd_` (input) from `do_cmd_` (execution)** — commands route over TCP to server
- Server owns all world state; clients are display terminals
- Ben Harrison's redraw flag system: any state change sets a flag, server re-sends to clients
- Used TCP for reliable ordered delivery

Limitations: Angband has small contained levels and limited simulation scope — orders of magnitude simpler than CDDA's open world.

---

## Key Technical Challenges

### 1. Single-Avatar Architecture
`get_avatar()` returns a singleton referenced in hundreds of files. Second player requires either:
- Converting `avatar` to support multiple instances (massive refactor)
- **NPC-wrapper approach:** Additional players are `npc`-like objects with special privileges (simpler, recommended starting point)

### 2. Reality Bubble
One 132×132 tile bubble, period. Two players 1km apart need two simultaneous simulation zones. Requires a bubble manager, decoupled map loading, and handling overlapping/diverging bubbles.

### 3. Long Actions and Time Skip
See above. Time negotiation protocol is the best known option.

### 4. State Synchronization Depth
A single turn can mutate: hundreds of inventory slots, vehicle parts, field effects (fire/acid spreading), NPC emotional states, map tiles, monster spawns, weather, sound propagation, and dozens of character stats. Any sync protocol must handle all of this.

### 5. NPC and Monster Ownership
Who owns a given NPC in a networked context? What happens when two bubbles overlap and both claim to update the same monster?

### 6. No Network Infrastructure
Starting from zero. Must choose:
- **Protocol:** TCP (required for roguelikes — ordered reliable delivery)
- **Serialization:** Extend existing JSON save system, or binary (msgpack/protobuf)
- **Architecture:** Authoritative server + thin clients vs. listen server vs. P2P
- **Security:** Anti-cheat, griefing prevention, save integrity

### 7. Vehicle System
CDDA vehicles are among the most complex objects in any roguelike — dozens of parts, individual health/fuel/electrical states, collision physics. Hard to synchronize.

### 8. Determinism
For action-only propagation (most bandwidth-efficient), the simulation must be deterministic across machines from the same seed + action sequence. CDDA's RNG usage in a multi-player context is unknown and likely not deterministic.

---

## Recommended Architecture (Research Consensus)

Based on MAngband's precedent and CDDA's specific constraints:

1. **Authoritative server model** — one server process owns world simulation; clients send action commands and receive world state deltas.

2. **NPC-wrapper for additional players** — represent human players internally as enhanced `npc` objects rather than multiple `avatar` instances. Minimizes refactor surface.

3. **Intercept at `handle_action()`** — route remote player commands through the existing input pipeline. This is the cleanest seam.

4. **Async real-time server** — the server ticks forward continuously and never pauses for player input. Long actions (sleep, crafting) resolve on their own while other players keep acting. Conflicting simultaneous actions resolved by server arrival order.

5. **Union reality bubble** — server maintains the union of all players' reality bubbles as the active simulation zone; per-player visibility is limited to their own bubble.

6. **TCP + extended JSON serialization** — use CDDA's existing JSON serialization for action commands and state deltas over TCP. Binary upgrade (msgpack) is a later optimization.

7. **Globally meaningful coordinates** — `tripoint_abs_ms` is the right type for all networked position data; it's already globally meaningful.

---

## Reference Links

- [CleverRaven/Cataclysm-DDA (upstream)](https://github.com/CleverRaven/Cataclysm-DDA)
- [CDDA Frequently Made Suggestions — Multiplayer](https://docs.cataclysmdda.org/FREQUENTLY_MADE_SUGGESTIONS.html)
- [CDDA Player Activity System](https://docs.cataclysmdda.org/PLAYER_ACTIVITY.html)
- [CDDA Points & Coordinates docs](https://github.com/CleverRaven/Cataclysm-DDA/blob/master/doc/c++/POINTS_COORDINATES.md)
- [GitHub #69634: More reality bubbles](https://github.com/CleverRaven/Cataclysm-DDA/issues/69634)
- [GitHub #55749: Multiplayer through time travel](https://github.com/CleverRaven/Cataclysm-DDA/issues/55749)
- [GitHub #81834: Hot seat multiplayer](https://github.com/CleverRaven/Cataclysm-DDA/issues/81834)
- [CataclysmLD (Python rewrite attempt)](https://github.com/stolencatkarma/CataclysmLD)
- [MAngband GitHub](https://github.com/mangband/mangband)
- [History of Multiplayer Roguelikes — Tangaria](https://tangaria.com/history/)
- [CDDA Architecture Analysis — TU Delft DESOSA 2019](https://se.ewi.tudelft.nl/desosa2019/chapters/cataclysm/)
- [DeepWiki: CDDA Architecture](https://deepwiki.com/CleverRaven/Cataclysm-DDA)
