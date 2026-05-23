# Cataclysm: Dark Days Ahead CO-OP?!

Why die alone when you can die together?

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

**macOS 11 (Big Sur) and older / Intel client** — Homebrew bottles on newer macOS may reference symbols not available on 11.x. Build natively on the target machine. Apple clang 12 doesn't recognize some GCC warning flags in the Makefile; silence them with `CXXFLAGS="-Wno-unknown-warning-option"`. pkg-config and sdl2-config live in `/usr/local/bin` which is not on the default SSH PATH — use `bash -l` when building over SSH:
```sh
brew install sdl2 sdl2_image sdl2_mixer sdl2_ttf freetype gettext ccache
# Local build:
CXXFLAGS="-Wno-unknown-warning-option" make -j$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 cataclysm-tiles
# Via SSH (always run in background — takes ~1 hour):
ssh ethankemp@192.168.1.25 "bash -l -c 'cd ~/Cataclysm-DDA-multi && touch src/mp_gamestate.cpp && make -j\$(sysctl -n hw.logicalcpu) TILES=1 SOUND=1 LINTJSON=0 PCH=0 CXXFLAGS=\"-Wno-unknown-warning-option\" cataclysm-tiles; echo EXIT:\$?'"
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
- Item pickup (single tile `g`, all nearby tiles `Q`)
- Item use and wielded-item use forwarded to server
- Host and client appear as NPC proxies in each other's world with correct clothing and skin tone
- Monster sync with damage messages
- Client ranged/thrown/spell damage forwarded to and applied on the server
- Field sync (blood, fire, acid)
- Tile sync (terrain, furniture, items, graffiti)
- Trap sync — client triggers traps on the server
- Vehicle driving by client (turning deducts AP; cruise speed is free)
- Vehicle state sync (part HP, fuel, name messages)

### Current limitations

- Only two players supported
- The sleep issue

---

## Frequently Asked Questions



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
