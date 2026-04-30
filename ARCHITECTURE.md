# Multiplayer Architecture

## Overview

Authoritative server model. One server process owns the world simulation; clients send action commands and receive world state updates. The server never trusts the client's view of game state — it only accepts intent ("I want to move north") and decides everything else.

**Maximum players: 2.** This is a hard design constraint for the initial implementation, not a technical limitation. Two players share a single reality bubble, simplifying the map, simulation, and sync systems dramatically. Expanding beyond 2 players and bursting the reality bubble are explicitly future work.

Turn-based mode first. Real-time mode later.

---

## Tech Stack

| Layer | Technology | Why |
|---|---|---|
| Transport | TCP | Reliable ordered delivery; turn-based doesn't need UDP's speed |
| Networking library | Asio standalone (header-only) | Single header drop-in, cross-platform, no new build deps, C++ standard-track |
| Serialization | nlohmann/json (already vendored) | Already in the codebase, human-readable, easy to debug |
| Server binary | Same binary, `--server` flag | One codebase, one build, no SDL needed in server mode |

**No new database. No cloud services. No game engine.** Saves stay in the existing CDDA JSON format on disk.

---

## The Message Protocol

Every message is a JSON object followed by a newline over TCP.

### Client → Server (action commands)

The client only sends intent. Never state.

```json
{"action": "move", "direction": "north"}
{"action": "pickup", "pos": {"x": 102, "y": 45, "z": 0}, "item": "baseball_bat"}
{"action": "craft", "recipe": "knife_crude"}
{"action": "attack", "target": "zombie_003"}
{"action": "sleep"}
```

### Server → Client (world state)

After every turn, each client receives a state update scoped to what their character can see. Not the full world — just their visible slice.

```json
{
  "turn": 15420,
  "your_character": {
    "hp": 72,
    "moves": 100,
    "pos": {"x": 102, "y": 45, "z": 0},
    "stamina": 890,
    "hunger": 4,
    "morale": -5,
    "status_effects": ["wet"],
    "inventory": []
  },
  "visible_tiles": [],
  "visible_creatures": [
    {"id": "zombie_003", "pos": {"x": 108, "y": 45, "z": 0}, "type": "zombie", "hp_pct": 0.8}
  ],
  "other_players": [
    {"name": "Player2", "pos": {"x": 99, "y": 42, "z": 0}}
  ],
  "messages": [
    "You hear a zombie moan to the east.",
    "Player2 picks up a crowbar."
  ]
}
```

The client populates its local game objects from this update and renders using the existing SDL/ncurses rendering code unchanged. The renderer doesn't know or care whether data came from a local simulation or a network packet.

---

## The Turn Cycle

The single-player loop blocks on keyboard input inside `handle_action()`. The multiplayer server loop blocks on network input instead. Everything else — monster turns, world updates, field effects, vehicle physics — runs unchanged.

```
Turn N begins
│
├── Server sends "awaiting_input" to all clients
│
├── Each client shows the player the current state, waits for a keypress
│   └── Player acts → client sends action JSON to server
│
├── Server collects one action per connected player
│   └── Player who doesn't respond within timeout: character idles
│
├── Server processes all player actions (arrival order for conflicts)
│
├── Server runs monster and NPC turns          ← unchanged from single-player
│
├── Server runs world updates (fire, decay, weather, vehicles)  ← unchanged
│
└── Server sends state update to each client → Turn N+1 begins
```

`collect_player_actions()` is a blocking wait with a timeout. No async complexity for turn-based. The only new code is at the top (collect) and bottom (broadcast) of the existing `do_turn()`.

---

## Long Actions in Turn-Based Mode (Sleep, Construction, Reading)

Long actions in CDDA are not simple waits — they have an active interrupt system. Sleep can be broken by asthma attacks, illness, drug comedowns, nightmares, nearby enemies, or environmental events (a zombie crashing through the window). These interrupts fire at specific turns during the action.

Turn compression doesn't work here. If Player A is on compressed turns while Player B takes normal turns, an interrupt event (zombie moose through the window) exists on Player A's timeline but not Player B's. That's a causality break, not a tuning problem.

**The solution for turn-based: shared time-skip with mutual consent.**

1. Player A initiates a long action (sleep, hours-long construction, etc.)
2. Server validates Player A can actually do it (not too stimulated, no immediate threats)
3. Server prompts Player B: "Player A wants to sleep. Join them?"
4. If Player B agrees: both fast-forward simultaneously — identical to single-player, except the interrupt system checks both players' conditions each tick
5. If any interrupt fires for either player, the skip pauses for both
6. If Player B rejects: Player A cannot sleep — same logic as a nearby enemy preventing sleep in single-player

This keeps the timeline consistent because both players advance through the same turns together. It also adds a natural cooperative layer — players must find a safe moment, secure a location, and agree to rest. That's co-op survival.

**Player B rejecting sleep is not a flaw.** You shouldn't be able to fast-forward 8 hours while your partner is in active combat.

In **real-time mode** (Phase 6) this problem doesn't exist — sleep runs at the fixed server tick rate, completes in ~28 real seconds, and the other player keeps playing. No negotiation needed.

---

## Connection Lifecycle

```
Client                              Server
  │                                    │
  │──── TCP connect ──────────────────▶│
  │◀─── {protocol: "cdda-mp",          │
  │      version: "0.1"}               │
  │                                    │
  │──── {type: "join",                 │
  │      name: "Alice",                │
  │      password: "hunter2"} ────────▶│
  │                                    │  validates, loads/creates character
  │◀─── {type: "welcome",              │
  │      player_id: "alice_01",        │
  │      world: "Riverside",           │
  │      current_turn: 15420}          │
  │                                    │
  │◀─── {type: "initial_state", ...}   │  full visible snapshot
  │                                    │
  │         [gameplay loop]            │
  │                                    │
  │──── {action: "quit"} ─────────────▶│
  │◀─── {type: "goodbye"} ─────────────│
  │──── TCP disconnect ────────────────│
```

**Disconnect handling:** If a client drops without a clean quit, the server detects the broken TCP connection and idles that character in the world. On reconnect the player resumes from the same position with the same character — same mechanism as the existing save/load system, triggered by network events instead of quit/launch.

---

## Multi-Player Representation

The `game` class holds `avatar &u` — a direct reference member, structurally one player. Additional players are represented as enhanced NPCs internally rather than adding a second `avatar`. This avoids touching the 462+ `get_avatar()` call sites and 375 `is_avatar()` checks immediately.

Additional players live in the NPC list with a flag marking them as human-controlled. The server routes their incoming action commands through the same `handle_action()` path that keyboard input uses today. This is the intercept point: `input_context` → `handle_action()` — instead of reading from a local keyboard, the server reads from a network buffer.

**Phase 1 constraint:** All players must remain within a single shared reality bubble (~132×132 tiles). This avoids the multi-bubble problem entirely for the initial implementation. Split bubbles are a later phase.

---

## The Binary

One binary, three modes:

```bash
# Single-player — unchanged, works exactly as today
./cataclysm-dda

# Server — headless, no SDL, opens a TCP port
./cataclysm-dda --server --port 8080 --world Riverside --password abc123

# Client — connects to a server instead of running locally
./cataclysm-dda --connect 192.168.1.100:8080 --name Alice
```

Server mode compiles without SDL. It loads a world from the saves folder the same way single-player does.

---

## Save Compatibility

- Server saves are **identical format** to single-player saves
- Any existing single-player world can be run as a multiplayer server
- Any multiplayer save can be played solo offline
- No migration, no conversion

The only addition is a small per-player character file for each connected player beyond the first. The world state file format is unchanged.

---

## Deployment

### Local / LAN

```bash
# macOS
brew install cmake ncurses
git clone https://github.com/busterbogheart/Cataclysm-DDA-multiplayer
cd Cataclysm-DDA-multiplayer
make SERVER=1 -j$(nproc)
./cataclysm-dda --server --port 8080 --world Riverside
```

Anyone on the local network connects to `192.168.1.x:8080`. For internet play, forward port 8080 on your router.

### VPS (persistent, internet)

Any provider — Hetzner (~€4/month), DigitalOcean ($4/month), etc. Ubuntu 22.04, 1 CPU, 1GB RAM.

```bash
apt install -y build-essential libncurses-dev git
git clone https://github.com/busterbogheart/Cataclysm-DDA-multiplayer /opt/cdda
cd /opt/cdda && make SERVER=1 -j$(nproc)
```

systemd service for persistence:

```ini
[Unit]
Description=CDDA Multiplayer Server
After=network.target

[Service]
ExecStart=/opt/cdda/cataclysm-dda --server --port 8080 --world Riverside
WorkingDirectory=/opt/cdda
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
systemctl enable cdda && systemctl start cdda
```

### Docker

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y build-essential libncurses-dev git
WORKDIR /cdda
COPY . .
RUN make SERVER=1 -j$(nproc)
EXPOSE 8080
VOLUME ["/cdda/save"]
CMD ["./cataclysm-dda", "--server", "--port", "8080"]
```

```bash
docker build -t cdda-server .
docker run -d \
  --name cdda \
  -p 8080:8080 \
  -v $(pwd)/saves:/cdda/save \
  --restart unless-stopped \
  cdda-server
```

The `-v` mount is critical — saves persist on the host and survive container rebuilds. Update the server by rebuilding the image and restarting; the world is untouched.

---

## Hardware Requirements (Server)

| Resource | Minimum | Notes |
|---|---|---|
| CPU | 1 core | Simulation is single-threaded |
| RAM | 1GB | Single bubble; more for multiple bubbles later |
| Disk | 512MB | World saves are small |
| Network | Any | Turn-based generates minimal bandwidth |
| GPU | None | Server is headless |
| OS | Linux | macOS/Windows possible but Linux recommended for servers |

---

## Implementation Phases

**Phase 1 — Network layer**
TCP server, connection lifecycle, command protocol, basic auth. No game changes yet. Verify two clients can connect and exchange messages.

**Phase 2 — Second player in world**
Represent connected players as enhanced NPCs. Route their action commands through `handle_action()`. Both players visible in the same world. Constraint: must stay within shared reality bubble.

**Phase 3 — Turn coordination**
Server collects actions from all players before advancing the turn. Idle timeout for slow/AFK players. Conflict resolution for simultaneous actions.

**Phase 4 — State broadcast**
Compute per-player visible state diffs after each turn. Send to clients. Client renders from received state.

**Phase 5 — Split reality bubbles**
Multiple players can roam independently. Server manages union of all active bubbles. This is the hardest phase.

**Phase 6 — Real-time mode**
Server ticks continuously regardless of player input. Players act asynchronously. Long actions resolve on their own while others keep playing.

---

## Release Formats

Matching upstream CDDA release targets:

| Binary | Build flags | Use |
|---|---|---|
| `cdda-osx-with-graphics-universal` | `TILES=1 SOUND=1 NATIVE=osx` | macOS client (Intel + Apple Silicon) |
| `cdda-osx-terminal-only-universal` | `NATIVE=osx` (no TILES) | macOS server (headless) |
| `cdda-linux-with-graphics-and-sounds-x64` | `TILES=1 SOUND=1 NATIVE=linux64` | Linux client |
| `cdda-linux-terminal-only-x64` | `NATIVE=linux64` (no TILES) | Linux server |
| Android APKs | `NATIVE=android` | Mobile client (future) |

The terminal-only (ncurses) build is the natural server binary — no SDL, smaller, headless. The `SERVER=1` Makefile flag will eventually produce this with server mode compiled in and debug menu disabled.

Upstream CI workflows live in `.github/workflows/` — fork and add multiplayer build flags to the matrix when ready to ship releases.
