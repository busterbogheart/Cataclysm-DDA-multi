# Cataclysm: Dark Days Ahead CO-OP?!

Why die alone when you can die together?

**Project home:** [cddacoop.com](https://cddacoop.com) &middot;
**Upstream:** [CleverRaven/Cataclysm-DDA](https://github.com/CleverRaven/Cataclysm-DDA)

## Contents

- [Multiplayer (This co-op fork)](#multiplayer-co-op-fork)
  - [Building (macOS)](#building-macos)
  - [Two-machine LAN quickstart](#two-machine-lan-quickstart)
  - [start-mp.sh modes](#start-mpsh-modes)
  - [Flag reference](#flag-reference)
  - [What works / Current limitations](#what-works)
- [FAQ](#frequently-asked-questions)
- [Contribute](#contribute)

## Multiplayer (This co-op fork)

This fork adds experimental co-op multiplayer. One player hosts; a second player connects as a client.

---

### Building (macOS)

Prerequisites (via Homebrew): `sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache`

**macOS 12+ (Monterey and newer):**
```sh
make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
```

**macOS 11 (Big Sur) and older / Intel** — Homebrew bottles on newer macOS may reference symbols not available on 11.x, so build natively on the target machine. Apple clang 12 doesn't recognize some GCC warning flags in the Makefile; silence them with `CXXFLAGS="-Wno-unknown-warning-option"`:
```sh
brew install sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache
CXXFLAGS="-Wno-unknown-warning-option" make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
```

Or, with MacPorts SDL via `pkg-config` and Clang:

```sh
export PKG_CONFIG_PATH="/opt/local/lib/pkgconfig"
make -j$(sysctl -n hw.logicalcpu) NATIVE=osx CLANG=1 TILES=1 SOUND=1 PCH=0 LINTJSON=0 cataclysm-tiles
```

---

### Two-machine LAN quickstart

Both machines need the binary built locally (binaries are CPU-architecture specific and can't be shared). The shared save is in the repo — `git pull` on the client syncs it automatically at session start.

Then use `start-mp.sh` — it handles world/character selection interactively and writes logs to `/tmp/`.

**Host machine** (plays the game, accepts one remote player):

```bash
./start-mp.sh host
```

**Client machine** (connects to the host):

```bash
./start-mp.sh client <host-lan-ip>
```

Both scripts prompt you to pick a world and character from the local save. The client teleports to the host's location on the first tick. Logs land at `/tmp/cdda-mp-server.log` and `/tmp/cdda-mp-client.log`.

**Playing across the internet:** [cddacoop.com](https://cddacoop.com) has step-by-step instructions for Tailscale, ZeroTier, ngrok, and router port forwarding. The host always listens on port 8080; only the route between the two machines changes.

---

### start-mp.sh modes

| Invocation | What it does |
|---|---|
| `./start-mp.sh` | Launches host + client on **this machine** — local two-window testing |
| `./start-mp.sh host` | Host only — full windowed game, listens on port 8080 |
| `./start-mp.sh client <ip>` | Client only — connects to `<ip>:8080` |

---

### Flag reference

| Flag | Argument | Default | Description |
|---|---|---|---|
| `--host` | — | — | Play normally while hosting a listen server |
| `--server` | — | — | Run as a headless dedicated server (no display) |
| `--world` | `<name>` | — | World to load on startup — required for host and client |
| `--char` | `<name>` | — | Character name to load |
| `--port` | `<number>` | 8080 | TCP port to listen on |
| `--password` | `<string>` | — | Password required to join |
| `--client` | `<host:port>` | — | Connect to a host as a client |
| `--client-name` | `<name>` | player2 | Player name shown to the host |

---

### What works

- Movement, melee combat, smashing terrain and furniture
- Item pickup (single tile `g`, all nearby tiles `Q`), drop, wear, wield
- Item use and wielded-item use forwarded to server
- Eating, drinking, and similar short consumption activities (both players can run them simultaneously)
- Host and client appear as NPC proxies in each other's world with correct clothing and skin tone
- Monster sync with damage messages
- Client ranged/thrown/spell damage forwarded to and applied on the server
- Field sync (blood, fire, acid)
- Tile sync (terrain, furniture, items, graffiti)
- Trap sync — client triggers traps on the server
- Vehicle driving by client (turning deducts AP; cruise speed is free)
- Vehicle state sync (part HP, fuel, name messages)
- Vehicle construction — install and remove parts, including parts spawned via debug
- Drop-into-vehicle (drop items into the storage of a vehicle you're standing on)
- **Co-op HUD** — bottom-left panel showing partner name, movement mode, worst-body-part HP bar, current activity + progress, and calendar drift
- **Partner menu** — bump into your partner to open a menu with "Tap on shoulder" and "Help with task" (helping engages SP's crafting helper math against the partner's synced stats)

### Current limitations

- Two players only — no third or fourth slot
- **One reality bubble**, centered on the host. A client more than ~66 tiles from the host falls outside the simulated area; entities there don't tick.
- **Sleep** is functional but two-player sleep dynamics are not yet validated — if one player sleeps and the other keeps acting, the world's lockstep is fine but expect rough edges around partner-status messages.
- **Long crafts (multi-hour)** work in same-machine testing; not yet validated across machines with real network latency.
- **No reconnect** — if the TCP connection drops, both players need to quit and re-launch.
- **Save format** is shared with upstream CDDA but the MP fork adds a few fields; saves between the fork and upstream are not interchangeable.

---

## Frequently Asked Questions

**Why a fork instead of a PR to upstream?**
The upstream project's lead developer has stated that multiplayer "simply can not be added" without overhauling most of the core game code. This fork is an experiment in doing exactly that without disrupting the upstream codebase. Where possible, MP-specific logic lives in `mp_*.cpp`/`mp_*.h` files so future upstream merges stay manageable.

**Does it run on Linux or Windows?**
Probably — the upstream build supports both — but only macOS has been tested against the multiplayer source. PRs welcome.

**Can I play with more than one friend?**
Not yet. The networking is hard-wired to one host + one client. A future change could allow more, but the reality-bubble limit (one simulated zone, host-centered) keeps everyone within ~66 tiles of the host regardless of slot count.

**Are saves between this fork and upstream CDDA interchangeable?**
No. The fork adds fields to the save format. Loading a fork save in upstream (or vice-versa) is not supported.

**My client disconnects mid-session — what now?**
For now: both quit, relaunch, and re-join. Automatic reconnection is on the roadmap.

**Is there a public test server?**
No. The architecture is peer-to-peer: one of the two players hosts directly. See [cddacoop.com](https://cddacoop.com) for how to expose the host to the other player.

---


## Contribute

The original upstream repo for Cataclysm: Dark Days Ahead is the result of contributions from over 1000 volunteers under the Creative Commons Attribution ShareAlike 3.0 license. The code and content of the game is free to use, modify, and redistribute for any purpose whatsoever. See https://creativecommons.org/licenses/by-sa/3.0/ for details.
Some code distributed with the project is not part of the project and is released under different software licenses; the files covered by different software licenses have their own license notices.

Please see [CONTRIBUTING.md](./CONTRIBUTING.md) for details.

Special thanks to the contributors, including but not limited to, people below:
<a href="https://github.com/cleverraven/cataclysm-dda/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=cleverraven/cataclysm-dda" />
</a>

Made with [contrib.rocks](https://contrib.rocks).

---
